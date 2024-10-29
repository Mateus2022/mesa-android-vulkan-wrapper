/*
 * Copyright © 2017 Red Hat
 *
 * SPDX-License-Identifier: MIT
 */
#include "radv_shader_info.h"
#include "nir/nir.h"
#include "nir/nir_xfb_info.h"
#include "nir/radv_nir.h"
#include "radv_device.h"
#include "radv_physical_device.h"
#include "radv_pipeline_graphics.h"
#include "radv_shader.h"

#include "ac_nir.h"

static void
mark_sampler_desc(const nir_variable *var, struct radv_shader_info *info)
{
   info->desc_set_used_mask |= (1u << var->data.descriptor_set);
}

static bool
radv_use_vs_prolog(const nir_shader *nir,
                   const struct radv_graphics_state_key *gfx_state)
{
   return gfx_state->vs.has_prolog && nir->info.inputs_read;
}

static bool
radv_use_per_attribute_vb_descs(const nir_shader *nir,
                                const struct radv_graphics_state_key *gfx_state,
                                const struct radv_shader_stage_key *stage_key)
{
   return stage_key->vertex_robustness1 || radv_use_vs_prolog(nir, gfx_state);
}

static void
gather_load_vs_input_info(const nir_shader *nir, const nir_intrinsic_instr *intrin, struct radv_shader_info *info,
                          const struct radv_graphics_state_key *gfx_state,
                          const struct radv_shader_stage_key *stage_key)
{
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const unsigned location = io_sem.location;
   const unsigned component = nir_intrinsic_component(intrin);
   unsigned mask = nir_def_components_read(&intrin->def);
   mask = (intrin->def.bit_size == 64 ? util_widen_mask(mask, 2) : mask) << component;

   if (location >= VERT_ATTRIB_GENERIC0) {
      const unsigned generic_loc = location - VERT_ATTRIB_GENERIC0;

      if (gfx_state->vi.instance_rate_inputs & BITFIELD_BIT(generic_loc)) {
         info->vs.needs_instance_id = true;
         info->vs.needs_base_instance = true;
      }

      if (radv_use_per_attribute_vb_descs(nir, gfx_state, stage_key))
         info->vs.vb_desc_usage_mask |= BITFIELD_BIT(generic_loc);
      else
         info->vs.vb_desc_usage_mask |= BITFIELD_BIT(gfx_state->vi.vertex_attribute_bindings[generic_loc]);

      info->vs.input_slot_usage_mask |= BITFIELD_RANGE(generic_loc, io_sem.num_slots);
   }
}

static void
gather_load_fs_input_info(const nir_shader *nir, const nir_intrinsic_instr *intrin, struct radv_shader_info *info,
                          const struct radv_graphics_state_key *gfx_state)
{
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const unsigned location = io_sem.location;
   const unsigned mapped_location = nir_intrinsic_base(intrin);
   const unsigned attrib_count = io_sem.num_slots;
   const unsigned component = nir_intrinsic_component(intrin);

   switch (location) {
   case VARYING_SLOT_CLIP_DIST0:
      info->ps.input_clips_culls_mask |= BITFIELD_RANGE(component, intrin->num_components);
      break;
   case VARYING_SLOT_CLIP_DIST1:
      info->ps.input_clips_culls_mask |= BITFIELD_RANGE(component, intrin->num_components) << 4;
      break;
   default:
      break;
   }

   const uint32_t mapped_mask = BITFIELD_RANGE(mapped_location, attrib_count);
   const bool per_primitive = nir->info.per_primitive_inputs & BITFIELD64_BIT(location);

   if (!per_primitive) {
      if (intrin->intrinsic == nir_intrinsic_load_input) {
         info->ps.flat_shaded_mask |= mapped_mask;
      } else if (intrin->intrinsic == nir_intrinsic_load_input_vertex) {
         if (io_sem.interp_explicit_strict)
            info->ps.explicit_strict_shaded_mask |= mapped_mask;
         else
            info->ps.explicit_shaded_mask |= mapped_mask;
      } else if (intrin->intrinsic == nir_intrinsic_load_interpolated_input && intrin->def.bit_size == 16) {
         if (io_sem.high_16bits)
            info->ps.float16_hi_shaded_mask |= mapped_mask;
         else
            info->ps.float16_shaded_mask |= mapped_mask;
      }
   }

   if (location >= VARYING_SLOT_VAR0) {
      const uint32_t var_mask = BITFIELD_RANGE(location - VARYING_SLOT_VAR0, attrib_count);

      if (per_primitive)
         info->ps.input_per_primitive_mask |= var_mask;
      else
         info->ps.input_mask |= var_mask;
   }
}

static void
gather_intrinsic_load_input_info(const nir_shader *nir, const nir_intrinsic_instr *instr, struct radv_shader_info *info,
                                 const struct radv_graphics_state_key *gfx_state,
                                 const struct radv_shader_stage_key *stage_key)
{
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      gather_load_vs_input_info(nir, instr, info, gfx_state, stage_key);
      break;
   case MESA_SHADER_FRAGMENT:
      gather_load_fs_input_info(nir, instr, info, gfx_state);
      break;
   default:
      break;
   }
}

static void
gather_intrinsic_store_output_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
                                   struct radv_shader_info *info, bool consider_force_vrs)
{
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(instr);
   const unsigned location = io_sem.location;
   const unsigned num_slots = io_sem.num_slots;
   const unsigned component = nir_intrinsic_component(instr);
   const unsigned write_mask = nir_intrinsic_write_mask(instr);
   uint8_t *output_usage_mask = NULL;

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      output_usage_mask = info->vs.output_usage_mask;
      break;
   case MESA_SHADER_TESS_EVAL:
      output_usage_mask = info->tes.output_usage_mask;
      break;
   case MESA_SHADER_GEOMETRY:
      output_usage_mask = info->gs.output_usage_mask;
      break;
   case MESA_SHADER_FRAGMENT:
      if (location >= FRAG_RESULT_DATA0) {
         const unsigned fs_semantic = location + io_sem.dual_source_blend_index;
         info->ps.colors_written |= 0xfu << (4 * (fs_semantic - FRAG_RESULT_DATA0));

         if (fs_semantic == FRAG_RESULT_DATA0)
            info->ps.color0_written = write_mask;
      }
      break;
   default:
      break;
   }

   if (output_usage_mask) {
      for (unsigned i = 0; i < num_slots; i++) {
         output_usage_mask[location + i] |= ((write_mask >> (i * 4)) & 0xf) << component;
      }
   }

   if (consider_force_vrs && location == VARYING_SLOT_POS) {
      unsigned pos_w_chan = 3 - component;

      if (write_mask & BITFIELD_BIT(pos_w_chan)) {
         nir_scalar pos_w = nir_scalar_resolved(instr->src[0].ssa, pos_w_chan);
         /* Use coarse shading if the value of Pos.W can't be determined or if its value is != 1
          * (typical for non-GUI elements).
          */
         if (!nir_scalar_is_const(pos_w) || nir_scalar_as_uint(pos_w) != 0x3f800000u)
            info->force_vrs_per_vertex = true;
      }
   }

   if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      const uint8_t gs_streams = nir_intrinsic_io_semantics(instr).gs_streams;
      info->gs.output_streams[location] |= gs_streams << (component * 2);
   }
}

static void
gather_push_constant_info(const nir_shader *nir, const nir_intrinsic_instr *instr, struct radv_shader_info *info)
{
   info->loads_push_constants = true;

   if (nir_src_is_const(instr->src[0]) && instr->def.bit_size >= 32) {
      uint32_t start = (nir_intrinsic_base(instr) + nir_src_as_uint(instr->src[0])) / 4u;
      uint32_t size = instr->num_components * (instr->def.bit_size / 32u);

      if (start + size <= (MAX_PUSH_CONSTANTS_SIZE / 4u)) {
         info->inline_push_constant_mask |= u_bit_consecutive64(start, size);
         return;
      }
   }

   info->can_inline_all_push_constants = false;
}

static void
gather_intrinsic_info(const nir_shader *nir, const nir_intrinsic_instr *instr, struct radv_shader_info *info,
                      const struct radv_graphics_state_key *gfx_state, const struct radv_shader_stage_key *stage_key,
                      bool consider_force_vrs)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_at_offset: {
      enum glsl_interp_mode mode = nir_intrinsic_interp_mode(instr);
      switch (mode) {
      case INTERP_MODE_SMOOTH:
      case INTERP_MODE_NONE:
         if (instr->intrinsic == nir_intrinsic_load_barycentric_pixel ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_sample ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_offset)
            info->ps.reads_persp_center = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_centroid)
            info->ps.reads_persp_centroid = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_sample)
            info->ps.reads_persp_sample = true;
         break;
      case INTERP_MODE_NOPERSPECTIVE:
         if (instr->intrinsic == nir_intrinsic_load_barycentric_pixel ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_sample ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_offset)
            info->ps.reads_linear_center = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_centroid)
            info->ps.reads_linear_centroid = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_sample)
            info->ps.reads_linear_sample = true;
         break;
      default:
         break;
      }
      if (instr->intrinsic == nir_intrinsic_load_barycentric_at_sample)
         info->ps.needs_sample_positions = true;
      break;
   }
   case nir_intrinsic_load_provoking_vtx_amd:
      info->ps.load_provoking_vtx = true;
      break;
   case nir_intrinsic_load_sample_positions_amd:
      info->ps.needs_sample_positions = true;
      break;
   case nir_intrinsic_load_rasterization_primitive_amd:
      info->ps.load_rasterization_prim = true;
      break;
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_workgroup_id: {
      unsigned mask = nir_def_components_read(&instr->def);
      while (mask) {
         unsigned i = u_bit_scan(&mask);

         if (instr->intrinsic == nir_intrinsic_load_workgroup_id)
            info->cs.uses_block_id[i] = true;
         else
            info->cs.uses_thread_id[i] = true;
      }
      break;
   }
   case nir_intrinsic_load_frag_coord:
      info->ps.reads_frag_coord_mask |= nir_def_components_read(&instr->def);
      break;
   case nir_intrinsic_load_sample_pos:
      info->ps.reads_sample_pos_mask |= nir_def_components_read(&instr->def);
      break;
   case nir_intrinsic_load_push_constant:
      gather_push_constant_info(nir, instr, info);
      break;
   case nir_intrinsic_vulkan_resource_index:
      info->desc_set_used_mask |= (1u << nir_intrinsic_desc_set(instr));
      break;
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples: {
      nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(instr->src[0].ssa->parent_instr));
      mark_sampler_desc(var, info);
      break;
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input_vertex:
      gather_intrinsic_load_input_info(nir, instr, info, gfx_state, stage_key);
      break;
   case nir_intrinsic_store_output:
      gather_intrinsic_store_output_info(nir, instr, info, consider_force_vrs);
      break;
   case nir_intrinsic_bvh64_intersect_ray_amd:
      info->cs.uses_rt = true;
      break;
   case nir_intrinsic_load_poly_line_smooth_enabled:
      info->ps.needs_poly_line_smooth = true;
      break;
   case nir_intrinsic_begin_invocation_interlock:
      info->ps.pops = true;
      break;
   default:
      break;
   }
}

static void
gather_tex_info(const nir_shader *nir, const nir_tex_instr *instr, struct radv_shader_info *info)
{
   for (unsigned i = 0; i < instr->num_srcs; i++) {
      switch (instr->src[i].src_type) {
      case nir_tex_src_texture_deref:
         mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
         break;
      case nir_tex_src_sampler_deref:
         mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
         break;
      default:
         break;
      }
   }
}

static void
gather_info_block(const nir_shader *nir, const nir_block *block, struct radv_shader_info *info,
                  const struct radv_graphics_state_key *gfx_state, const struct radv_shader_stage_key *stage_key,
                  bool consider_force_vrs)
{
   nir_foreach_instr (instr, block) {
      switch (instr->type) {
      case nir_instr_type_intrinsic:
         gather_intrinsic_info(nir, nir_instr_as_intrinsic(instr), info, gfx_state, stage_key, consider_force_vrs);
         break;
      case nir_instr_type_tex:
         gather_tex_info(nir, nir_instr_as_tex(instr), info);
         break;
      default:
         break;
      }
   }
}

static void
gather_xfb_info(const nir_shader *nir, struct radv_shader_info *info)
{
   struct radv_streamout_info *so = &info->so;

   if (!nir->xfb_info)
      return;

   const nir_xfb_info *xfb = nir->xfb_info;
   assert(xfb->output_count <= MAX_SO_OUTPUTS);
   so->num_outputs = xfb->output_count;

   for (unsigned i = 0; i < xfb->output_count; i++) {
      unsigned output_buffer = xfb->outputs[i].buffer;
      unsigned stream = xfb->buffer_to_stream[xfb->outputs[i].buffer];
      so->enabled_stream_buffers_mask |= (1 << output_buffer) << (stream * 4);
   }

   for (unsigned i = 0; i < NIR_MAX_XFB_BUFFERS; i++) {
      so->strides[i] = xfb->buffers[i].stride / 4;
   }
}

static void
assign_outinfo_param(struct radv_vs_output_info *outinfo, gl_varying_slot idx, unsigned *total_param_exports,
                     unsigned extra_offset)
{
   if (outinfo->vs_output_param_offset[idx] == AC_EXP_PARAM_UNDEFINED)
      outinfo->vs_output_param_offset[idx] = extra_offset + (*total_param_exports)++;
}

static void
assign_outinfo_params(struct radv_vs_output_info *outinfo, uint64_t mask, unsigned *total_param_exports,
                      unsigned extra_offset)
{
   u_foreach_bit64 (idx, mask) {
      if (idx >= VARYING_SLOT_VAR0 || idx == VARYING_SLOT_LAYER || idx == VARYING_SLOT_PRIMITIVE_ID ||
          idx == VARYING_SLOT_VIEWPORT)
         assign_outinfo_param(outinfo, idx, total_param_exports, extra_offset);
   }
}

static void
radv_get_output_masks(const struct nir_shader *nir, const struct radv_graphics_state_key *gfx_state,
                      uint64_t *per_vtx_mask, uint64_t *per_prim_mask)
{
   /* These are not compiled into neither output param nor position exports. */
   const uint64_t special_mask = BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_COUNT) |
                                 BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES) |
                                 BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE);

   *per_prim_mask = nir->info.outputs_written & nir->info.per_primitive_outputs & ~special_mask;
   *per_vtx_mask = nir->info.outputs_written & ~nir->info.per_primitive_outputs & ~special_mask;

   /* Mesh multiview is only lowered in ac_nir_lower_ngg, so we have to fake it here. */
   if (nir->info.stage == MESA_SHADER_MESH && gfx_state->has_multiview_view_index)
      *per_prim_mask |= VARYING_BIT_LAYER;
}

static void
radv_set_vs_output_param(struct radv_device *device, const struct nir_shader *nir,
                         const struct radv_graphics_state_key *gfx_state, struct radv_shader_info *info,
                         bool export_prim_id, bool export_clip_cull_dists)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_vs_output_info *outinfo = &info->outinfo;
   uint64_t per_vtx_mask, per_prim_mask;

   radv_get_output_masks(nir, gfx_state, &per_vtx_mask, &per_prim_mask);

   memset(outinfo->vs_output_param_offset, AC_EXP_PARAM_UNDEFINED, sizeof(outinfo->vs_output_param_offset));

   unsigned total_param_exports = 0;

   /* Per-vertex outputs */
   assign_outinfo_params(outinfo, per_vtx_mask, &total_param_exports, 0);

   if (export_prim_id && (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL)) {
      /* Mark the primitive ID as output when it's implicitly exported by VS or TES. */
      if (outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] == AC_EXP_PARAM_UNDEFINED)
         outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] = total_param_exports++;

      outinfo->export_prim_id = true;
   }

   if (export_clip_cull_dists) {
      if (nir->info.outputs_written & VARYING_BIT_CLIP_DIST0)
         outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST0] = total_param_exports++;
      if (nir->info.outputs_written & VARYING_BIT_CLIP_DIST1)
         outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST1] = total_param_exports++;
   }

   outinfo->param_exports = total_param_exports;

   /* The HW always assumes that there is at least 1 per-vertex param.
    * so if there aren't any, we have to offset per-primitive params by 1.
    */
   const unsigned extra_offset = !!(total_param_exports == 0 && pdev->info.gfx_level >= GFX11);

   /* Per-primitive outputs: the HW needs these to be last. */
   assign_outinfo_params(outinfo, per_prim_mask, &total_param_exports, extra_offset);

   outinfo->prim_param_exports = total_param_exports - outinfo->param_exports;
}

static uint8_t
radv_get_wave_size(struct radv_device *device, gl_shader_stage stage, const struct radv_shader_info *info,
                   const struct radv_shader_stage_key *stage_key)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (stage_key->subgroup_required_size)
      return stage_key->subgroup_required_size * 32;

   if (stage == MESA_SHADER_GEOMETRY && !info->is_ngg)
      return 64;
   else if (stage == MESA_SHADER_COMPUTE || stage == MESA_SHADER_TASK)
      return info->wave_size;
   else if (stage == MESA_SHADER_FRAGMENT)
      return pdev->ps_wave_size;
   else if (gl_shader_stage_is_rt(stage))
      return pdev->rt_wave_size;
   else
      return pdev->ge_wave_size;
}

static uint8_t
radv_get_ballot_bit_size(struct radv_device *device, gl_shader_stage stage, const struct radv_shader_info *info,
                         const struct radv_shader_stage_key *stage_key)
{
   if (stage_key->subgroup_required_size)
      return stage_key->subgroup_required_size * 32;

   return 64;
}

static uint32_t
radv_compute_esgs_itemsize(const struct radv_device *device, uint32_t num_varyings)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t esgs_itemsize;

   esgs_itemsize = num_varyings * 16;

   /* For the ESGS ring in LDS, add 1 dword to reduce LDS bank
    * conflicts, i.e. each vertex will start on a different bank.
    */
   if (pdev->info.gfx_level >= GFX9 && esgs_itemsize)
      esgs_itemsize += 4;

   return esgs_itemsize;
}

static void
gather_shader_info_ngg_query(struct radv_device *device, struct radv_shader_info *info)
{
   info->has_xfb_query = info->so.num_outputs > 0;
   info->has_prim_query = device->cache_key.primitives_generated_query || info->has_xfb_query;
}

uint64_t
radv_gather_unlinked_io_mask(const uint64_t nir_io_mask)
{
   /* Create a mask of driver locations mapped from NIR semantics. */
   uint64_t radv_io_mask = 0;
   u_foreach_bit64 (semantic, nir_io_mask) {
      /* These outputs are not used when fixed output slots are needed. */
      if (semantic == VARYING_SLOT_LAYER || semantic == VARYING_SLOT_VIEWPORT ||
          semantic == VARYING_SLOT_PRIMITIVE_ID || semantic == VARYING_SLOT_PRIMITIVE_SHADING_RATE)
         continue;

      radv_io_mask |= BITFIELD64_BIT(radv_map_io_driver_location(semantic));
   }

   return radv_io_mask;
}

uint64_t
radv_gather_unlinked_patch_io_mask(const uint64_t nir_io_mask, const uint32_t nir_patch_io_mask)
{
   uint64_t radv_io_mask = 0;
   u_foreach_bit64 (semantic, nir_patch_io_mask) {
      radv_io_mask |= BITFIELD64_BIT(radv_map_io_driver_location(semantic + VARYING_SLOT_PATCH0));
   }

   /* Tess levels need to be handled separately because they are not part of patch_outputs_written. */
   if (nir_io_mask & VARYING_BIT_TESS_LEVEL_OUTER)
      radv_io_mask |= BITFIELD64_BIT(radv_map_io_driver_location(VARYING_SLOT_TESS_LEVEL_OUTER));
   if (nir_io_mask & VARYING_BIT_TESS_LEVEL_INNER)
      radv_io_mask |= BITFIELD64_BIT(radv_map_io_driver_location(VARYING_SLOT_TESS_LEVEL_INNER));

   return radv_io_mask;
}

static void
gather_shader_info_vs(struct radv_device *device, const nir_shader *nir,
                      const struct radv_graphics_state_key *gfx_state, const struct radv_shader_stage_key *stage_key,
                      struct radv_shader_info *info)
{
   if (radv_use_vs_prolog(nir, gfx_state)) {
      info->vs.has_prolog = true;
      info->vs.dynamic_inputs = true;
   }

   /* Use per-attribute vertex descriptors to prevent faults and for correct bounds checking. */
   info->vs.use_per_attribute_vb_descs = radv_use_per_attribute_vb_descs(nir, gfx_state, stage_key);

   /* We have to ensure consistent input register assignments between the main shader and the
    * prolog.
    */
   info->vs.needs_instance_id |= info->vs.has_prolog;
   info->vs.needs_base_instance |= info->vs.has_prolog;
   info->vs.needs_draw_id |= info->vs.has_prolog;

   if (info->vs.dynamic_inputs)
      info->vs.vb_desc_usage_mask = BITFIELD_MASK(util_last_bit(info->vs.vb_desc_usage_mask));

   /* When the topology is unknown (with GPL), the number of vertices per primitive needs be passed
    * through a user SGPR for NGG streamout with VS. Otherwise, the XFB offset is incorrectly
    * computed because using the maximum number of vertices can't work.
    */
   info->vs.dynamic_num_verts_per_prim = gfx_state->ia.topology == V_008958_DI_PT_NONE && info->is_ngg && nir->xfb_info;

   if (!info->outputs_linked)
      info->vs.num_linked_outputs = util_last_bit64(radv_gather_unlinked_io_mask(nir->info.outputs_written));

   if (info->next_stage == MESA_SHADER_TESS_CTRL) {
      info->vs.as_ls = true;
   } else if (info->next_stage == MESA_SHADER_GEOMETRY) {
      info->vs.as_es = true;
      info->esgs_itemsize = radv_compute_esgs_itemsize(device, info->vs.num_linked_outputs);
   }

   if (info->is_ngg) {
      info->vs.num_outputs = nir->num_outputs;

      if (info->next_stage == MESA_SHADER_FRAGMENT || info->next_stage == MESA_SHADER_NONE) {
         gather_shader_info_ngg_query(device, info);
      }
   }
}

static void
gather_shader_info_tcs(struct radv_device *device, const nir_shader *nir,
                       const struct radv_graphics_state_key *gfx_state, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   const uint64_t tess_lvl_mask = VARYING_BIT_TESS_LEVEL_OUTER | VARYING_BIT_TESS_LEVEL_INNER;
   const uint64_t per_vtx_out_mask = nir->info.outputs_read & nir->info.outputs_written & ~tess_lvl_mask;
   const uint64_t tess_lvl_out_mask = nir->info.outputs_written & tess_lvl_mask;
   const uint32_t per_patch_out_mask = nir->info.patch_outputs_read & nir->info.patch_outputs_written;

   info->tcs.num_lds_per_vertex_outputs = util_bitcount64(per_vtx_out_mask);
   info->tcs.num_lds_per_patch_outputs = util_bitcount64(tess_lvl_out_mask) + util_bitcount(per_patch_out_mask);
   info->tcs.tcs_vertices_out = nir->info.tess.tcs_vertices_out;
   info->tcs.tes_inputs_read = ~0ULL;
   info->tcs.tes_patch_inputs_read = ~0ULL;

   if (!info->inputs_linked)
      info->tcs.num_linked_inputs = util_last_bit64(radv_gather_unlinked_io_mask(nir->info.inputs_read));
   if (!info->outputs_linked) {
      info->tcs.num_linked_outputs = util_last_bit64(radv_gather_unlinked_io_mask(
         nir->info.outputs_written & ~(VARYING_BIT_TESS_LEVEL_OUTER | VARYING_BIT_TESS_LEVEL_INNER)));
      info->tcs.num_linked_patch_outputs = util_last_bit64(
         radv_gather_unlinked_patch_io_mask(nir->info.outputs_written, nir->info.patch_outputs_written));
   }

   if (gfx_state->ts.patch_control_points) {
      /* Number of tessellation patches per workgroup processed by the current pipeline. */
      info->num_tess_patches = radv_get_tcs_num_patches(
         pdev, gfx_state->ts.patch_control_points, nir->info.tess.tcs_vertices_out, info->tcs.num_linked_inputs,
         info->tcs.num_lds_per_vertex_outputs, info->tcs.num_lds_per_patch_outputs, info->tcs.num_linked_outputs,
         info->tcs.num_linked_patch_outputs);

      /* LDS size used by VS+TCS for storing TCS inputs and outputs. */
      info->tcs.num_lds_blocks = radv_get_tess_lds_size(
         pdev, gfx_state->ts.patch_control_points, nir->info.tess.tcs_vertices_out, info->tcs.num_linked_inputs,
         info->num_tess_patches, info->tcs.num_lds_per_vertex_outputs, info->tcs.num_lds_per_patch_outputs);
   }
}

static void
gather_shader_info_tes(struct radv_device *device, const nir_shader *nir, struct radv_shader_info *info)
{
   info->tes._primitive_mode = nir->info.tess._primitive_mode;
   info->tes.spacing = nir->info.tess.spacing;
   info->tes.ccw = nir->info.tess.ccw;
   info->tes.point_mode = nir->info.tess.point_mode;
   info->tes.tcs_vertices_out = nir->info.tess.tcs_vertices_out;
   info->tes.reads_tess_factors =
      !!(nir->info.inputs_read & (VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER));

   if (!info->inputs_linked) {
      info->tes.num_linked_inputs = util_last_bit64(radv_gather_unlinked_io_mask(
         nir->info.inputs_read & ~(VARYING_BIT_TESS_LEVEL_OUTER | VARYING_BIT_TESS_LEVEL_INNER)));
      info->tes.num_linked_patch_inputs = util_last_bit64(
         radv_gather_unlinked_patch_io_mask(nir->info.inputs_read, nir->info.patch_inputs_read));
   }
   if (!info->outputs_linked)
      info->tes.num_linked_outputs = util_last_bit64(radv_gather_unlinked_io_mask(nir->info.outputs_written));

   if (info->next_stage == MESA_SHADER_GEOMETRY) {
      info->tes.as_es = true;
      info->esgs_itemsize = radv_compute_esgs_itemsize(device, info->tes.num_linked_outputs);
   }

   if (info->is_ngg) {
      info->tes.num_outputs = nir->num_outputs;

      if (info->next_stage == MESA_SHADER_FRAGMENT || info->next_stage == MESA_SHADER_NONE) {
         gather_shader_info_ngg_query(device, info);
      }
   }
}

static void
radv_init_legacy_gs_ring_info(const struct radv_device *device, struct radv_shader_info *gs_info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_legacy_gs_info *gs_ring_info = &gs_info->gs_ring_info;
   unsigned num_se = pdev->info.max_se;
   unsigned wave_size = 64;
   unsigned max_gs_waves = 32 * num_se; /* max 32 per SE on GCN */
   /* On GFX6-GFX7, the value comes from VGT_GS_VERTEX_REUSE = 16.
    * On GFX8+, the value comes from VGT_VERTEX_REUSE_BLOCK_CNTL = 30 (+2).
    */
   unsigned gs_vertex_reuse = (pdev->info.gfx_level >= GFX8 ? 32 : 16) * num_se;
   unsigned alignment = 256 * num_se;
   /* The maximum size is 63.999 MB per SE. */
   unsigned max_size = ((unsigned)(63.999 * 1024 * 1024) & ~255) * num_se;

   /* Calculate the minimum size. */
   unsigned min_esgs_ring_size = align(gs_ring_info->esgs_itemsize * 4 * gs_vertex_reuse * wave_size, alignment);
   /* These are recommended sizes, not minimum sizes. */
   unsigned esgs_ring_size = max_gs_waves * 2 * wave_size * gs_ring_info->esgs_itemsize * 4 * gs_info->gs.vertices_in;
   unsigned gsvs_ring_size = max_gs_waves * 2 * wave_size * gs_info->gs.max_gsvs_emit_size;

   min_esgs_ring_size = align(min_esgs_ring_size, alignment);
   esgs_ring_size = align(esgs_ring_size, alignment);
   gsvs_ring_size = align(gsvs_ring_size, alignment);

   if (pdev->info.gfx_level <= GFX8)
      gs_ring_info->esgs_ring_size = CLAMP(esgs_ring_size, min_esgs_ring_size, max_size);

   gs_ring_info->gsvs_ring_size = MIN2(gsvs_ring_size, max_size);
}

static void
radv_get_legacy_gs_info(const struct radv_device *device, struct radv_shader_info *gs_info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_legacy_gs_info *out = &gs_info->gs_ring_info;
   const unsigned gs_num_invocations = MAX2(gs_info->gs.invocations, 1);
   const bool uses_adjacency =
      gs_info->gs.input_prim == MESA_PRIM_LINES_ADJACENCY || gs_info->gs.input_prim == MESA_PRIM_TRIANGLES_ADJACENCY;

   /* All these are in dwords: */
   /* We can't allow using the whole LDS, because GS waves compete with
    * other shader stages for LDS space. */
   const unsigned max_lds_size = 8 * 1024;
   const unsigned esgs_itemsize = radv_compute_esgs_itemsize(device, gs_info->gs.num_linked_inputs) / 4;
   unsigned esgs_lds_size;

   /* All these are per subgroup: */
   const unsigned max_out_prims = 32 * 1024;
   const unsigned max_es_verts = 255;
   const unsigned ideal_gs_prims = 64;
   unsigned max_gs_prims, gs_prims;
   unsigned min_es_verts, es_verts, worst_case_es_verts;

   if (uses_adjacency || gs_num_invocations > 1)
      max_gs_prims = 127 / gs_num_invocations;
   else
      max_gs_prims = 255;

   /* MAX_PRIMS_PER_SUBGROUP = gs_prims * max_vert_out * gs_invocations.
    * Make sure we don't go over the maximum value.
    */
   if (gs_info->gs.vertices_out > 0) {
      max_gs_prims = MIN2(max_gs_prims, max_out_prims / (gs_info->gs.vertices_out * gs_num_invocations));
   }
   assert(max_gs_prims > 0);

   /* If the primitive has adjacency, halve the number of vertices
    * that will be reused in multiple primitives.
    */
   min_es_verts = gs_info->gs.vertices_in / (uses_adjacency ? 2 : 1);

   gs_prims = MIN2(ideal_gs_prims, max_gs_prims);
   worst_case_es_verts = MIN2(min_es_verts * gs_prims, max_es_verts);

   /* Compute ESGS LDS size based on the worst case number of ES vertices
    * needed to create the target number of GS prims per subgroup.
    */
   esgs_lds_size = esgs_itemsize * worst_case_es_verts;

   /* If total LDS usage is too big, refactor partitions based on ratio
    * of ESGS item sizes.
    */
   if (esgs_lds_size > max_lds_size) {
      /* Our target GS Prims Per Subgroup was too large. Calculate
       * the maximum number of GS Prims Per Subgroup that will fit
       * into LDS, capped by the maximum that the hardware can support.
       */
      gs_prims = MIN2((max_lds_size / (esgs_itemsize * min_es_verts)), max_gs_prims);
      assert(gs_prims > 0);
      worst_case_es_verts = MIN2(min_es_verts * gs_prims, max_es_verts);

      esgs_lds_size = esgs_itemsize * worst_case_es_verts;
      assert(esgs_lds_size <= max_lds_size);
   }

   /* Now calculate remaining ESGS information. */
   if (esgs_lds_size)
      es_verts = MIN2(esgs_lds_size / esgs_itemsize, max_es_verts);
   else
      es_verts = max_es_verts;

   /* Vertices for adjacency primitives are not always reused, so restore
    * it for ES_VERTS_PER_SUBGRP.
    */
   min_es_verts = gs_info->gs.vertices_in;

   /* For normal primitives, the VGT only checks if they are past the ES
    * verts per subgroup after allocating a full GS primitive and if they
    * are, kick off a new subgroup.  But if those additional ES verts are
    * unique (e.g. not reused) we need to make sure there is enough LDS
    * space to account for those ES verts beyond ES_VERTS_PER_SUBGRP.
    */
   es_verts -= min_es_verts - 1;

   const uint32_t es_verts_per_subgroup = es_verts;
   const uint32_t gs_prims_per_subgroup = gs_prims;
   const uint32_t gs_inst_prims_in_subgroup = gs_prims * gs_num_invocations;
   const uint32_t max_prims_per_subgroup = gs_inst_prims_in_subgroup * gs_info->gs.vertices_out;
   const uint32_t lds_granularity = pdev->info.lds_encode_granularity;
   const uint32_t total_lds_bytes = align(esgs_lds_size * 4, lds_granularity);

   out->gs_inst_prims_in_subgroup = gs_inst_prims_in_subgroup;
   out->es_verts_per_subgroup = es_verts_per_subgroup;
   out->gs_prims_per_subgroup = gs_prims_per_subgroup;
   out->esgs_itemsize = esgs_itemsize;
   out->lds_size = total_lds_bytes / lds_granularity;
   assert(max_prims_per_subgroup <= max_out_prims);

   radv_init_legacy_gs_ring_info(device, gs_info);
}

static void
gather_shader_info_gs(struct radv_device *device, const nir_shader *nir, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned add_clip = nir->info.clip_distance_array_size + nir->info.cull_distance_array_size > 4;
   info->gs.gsvs_vertex_size = (util_bitcount64(nir->info.outputs_written) + add_clip) * 16;
   info->gs.max_gsvs_emit_size = info->gs.gsvs_vertex_size * nir->info.gs.vertices_out;

   info->gs.vertices_in = nir->info.gs.vertices_in;
   info->gs.vertices_out = nir->info.gs.vertices_out;
   info->gs.input_prim = nir->info.gs.input_primitive;
   info->gs.output_prim = nir->info.gs.output_primitive;
   info->gs.invocations = nir->info.gs.invocations;
   info->gs.max_stream = nir->info.gs.active_stream_mask ? util_last_bit(nir->info.gs.active_stream_mask) - 1 : 0;
   info->gs.has_pipeline_stat_query = pdev->emulate_ngg_gs_query_pipeline_stat;

   for (unsigned slot = 0; slot < VARYING_SLOT_MAX; ++slot) {
      const uint8_t usage_mask = info->gs.output_usage_mask[slot];
      const uint8_t gs_streams = info->gs.output_streams[slot];

      for (unsigned component = 0; component < 4; ++component) {
         if (!(usage_mask & BITFIELD_BIT(component)))
            continue;

         const uint8_t stream = (gs_streams >> (component * 2)) & 0x3;
         info->gs.num_stream_output_components[stream]++;
      }
   }

   if (!info->inputs_linked)
      info->gs.num_linked_inputs = util_last_bit64(radv_gather_unlinked_io_mask(nir->info.inputs_read));

   if (info->is_ngg) {
      gather_shader_info_ngg_query(device, info);
   } else {
      radv_get_legacy_gs_info(device, info);
   }
}

static void
gather_shader_info_mesh(struct radv_device *device, const nir_shader *nir,
                        const struct radv_shader_stage_key *stage_key, struct radv_shader_info *info)
{
   struct gfx10_ngg_info *ngg_info = &info->ngg_info;

   info->ms.output_prim = nir->info.mesh.primitive_type;

   /* Special case for mesh shader workgroups.
    *
    * Mesh shaders don't have any real vertex input, but they can produce
    * an arbitrary number of vertices and primitives (up to 256).
    * We need to precisely control the number of mesh shader workgroups
    * that are launched from draw calls.
    *
    * To achieve that, we set:
    * - input primitive topology to point list
    * - input vertex and primitive count to 1
    * - max output vertex count and primitive amplification factor
    *   to the boundaries of the shader
    *
    * With that, in the draw call:
    * - drawing 1 input vertex ~ launching 1 mesh shader workgroup
    *
    * In the shader:
    * - input vertex id ~ workgroup id (in 1D - shader needs to calculate in 3D)
    *
    * Notes:
    * - without GS_EN=1 PRIM_AMP_FACTOR and MAX_VERTS_PER_SUBGROUP don't seem to work
    * - with GS_EN=1 we must also set VGT_GS_MAX_VERT_OUT (otherwise the GPU hangs)
    * - with GS_FAST_LAUNCH=1 every lane's VGPRs are initialized to the same input vertex index
    *
    */
   ngg_info->esgs_ring_size = 1;
   ngg_info->hw_max_esverts = 1;
   ngg_info->max_gsprims = 1;
   ngg_info->max_out_verts = nir->info.mesh.max_vertices_out;
   ngg_info->max_vert_out_per_gs_instance = false;
   ngg_info->ngg_emit_size = 0;
   ngg_info->prim_amp_factor = nir->info.mesh.max_primitives_out;
   ngg_info->vgt_esgs_ring_itemsize = 1;

   info->ms.has_query = device->cache_key.mesh_shader_queries;
   info->ms.has_task = stage_key->has_task_shader;
}

static void
calc_mesh_workgroup_size(const struct radv_device *device, const nir_shader *nir, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned api_workgroup_size = ac_compute_cs_workgroup_size(nir->info.workgroup_size, false, UINT32_MAX);

   if (pdev->mesh_fast_launch_2) {
      /* Use multi-row export. It is also necessary to use the API workgroup size for non-emulated queries. */
      info->workgroup_size = api_workgroup_size;
   } else {
      struct gfx10_ngg_info *ngg_info = &info->ngg_info;
      unsigned min_ngg_workgroup_size = ac_compute_ngg_workgroup_size(
         ngg_info->hw_max_esverts, ngg_info->max_gsprims, ngg_info->max_out_verts, ngg_info->prim_amp_factor);

      info->workgroup_size = MAX2(min_ngg_workgroup_size, api_workgroup_size);
   }
}

static void
gather_shader_info_fs(const struct radv_device *device, const nir_shader *nir,
                      const struct radv_graphics_state_key *gfx_state, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t per_primitive_input_mask = nir->info.inputs_read & nir->info.per_primitive_inputs;
   const unsigned num_per_primitive_inputs = util_bitcount64(per_primitive_input_mask);
   const unsigned num_inputs = util_bitcount64(nir->info.inputs_read);
   assert(num_per_primitive_inputs <= num_inputs);

   info->ps.num_interp = num_inputs;
   info->ps.num_prim_interp = 0;

   if (pdev->info.gfx_level == GFX10_3) {
      /* GFX10.3 distinguishes NUM_INTERP and NUM_PRIM_INTERP, but
       * these are counted together in NUM_INTERP on GFX11.
       */
      info->ps.num_interp = num_inputs - num_per_primitive_inputs;
      info->ps.num_prim_interp = num_per_primitive_inputs;
   }

   info->ps.can_discard = nir->info.fs.uses_discard;
   info->ps.early_fragment_test =
      nir->info.fs.early_fragment_tests ||
      (nir->info.fs.early_and_late_fragment_tests && nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_NONE &&
       nir->info.fs.stencil_front_layout == FRAG_STENCIL_LAYOUT_NONE &&
       nir->info.fs.stencil_back_layout == FRAG_STENCIL_LAYOUT_NONE);
   info->ps.post_depth_coverage = nir->info.fs.post_depth_coverage;
   info->ps.depth_layout = nir->info.fs.depth_layout;
   info->ps.uses_sample_shading = nir->info.fs.uses_sample_shading;
   info->ps.writes_memory = nir->info.writes_memory;
   info->ps.has_pcoord = nir->info.inputs_read & VARYING_BIT_PNTC;
   info->ps.prim_id_input = nir->info.inputs_read & VARYING_BIT_PRIMITIVE_ID;
   info->ps.layer_input = nir->info.inputs_read & VARYING_BIT_LAYER;
   info->ps.viewport_index_input = nir->info.inputs_read & VARYING_BIT_VIEWPORT;
   info->ps.writes_z = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH);
   info->ps.writes_stencil = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL);
   info->ps.writes_sample_mask = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK);
   info->ps.reads_sample_mask_in = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN);
   info->ps.reads_sample_id = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID);
   info->ps.reads_frag_shading_rate = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_SHADING_RATE);
   info->ps.reads_front_face = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRONT_FACE);
   info->ps.reads_barycentric_model = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL);
   info->ps.reads_fully_covered = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FULLY_COVERED);

   bool uses_persp_or_linear_interp = info->ps.reads_persp_center || info->ps.reads_persp_centroid ||
                                      info->ps.reads_persp_sample || info->ps.reads_linear_center ||
                                      info->ps.reads_linear_centroid || info->ps.reads_linear_sample;

   info->ps.allow_flat_shading =
      !(uses_persp_or_linear_interp || info->ps.needs_sample_positions || info->ps.reads_frag_shading_rate ||
        info->ps.writes_memory || nir->info.fs.needs_quad_helper_invocations ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_POINT_COORD) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_POS) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_HELPER_INVOCATION));

   info->ps.pops_is_per_sample =
      info->ps.pops && (nir->info.fs.sample_interlock_ordered || nir->info.fs.sample_interlock_unordered);

   info->ps.spi_ps_input_ena = radv_compute_spi_ps_input(pdev, gfx_state, info);
   info->ps.spi_ps_input_addr = info->ps.spi_ps_input_ena;
   if (pdev->info.gfx_level >= GFX12) {
      /* Only SPI_PS_INPUT_ENA has this bit on GFX12. */
      info->ps.spi_ps_input_addr &= C_02865C_COVERAGE_TO_SHADER_SELECT;
   }

   info->has_epilog = gfx_state->ps.has_epilog && info->ps.colors_written;

   if (!info->has_epilog) {
      info->ps.mrt0_is_dual_src = gfx_state->ps.epilog.mrt0_is_dual_src;
      info->ps.spi_shader_col_format = gfx_state->ps.epilog.spi_shader_col_format;

      /* Clear color attachments that aren't exported by the FS to match IO shader arguments. */
      info->ps.spi_shader_col_format &= info->ps.colors_written;

      info->ps.cb_shader_mask = ac_get_cb_shader_mask(info->ps.spi_shader_col_format);
   }

   const bool export_alpha_and_mrtz =
      (info->ps.color0_written & 0x8) && (info->ps.writes_z || info->ps.writes_stencil || info->ps.writes_sample_mask);

   info->ps.exports_mrtz_via_epilog =
      info->has_epilog && gfx_state->ps.exports_mrtz_via_epilog && export_alpha_and_mrtz;

   if (!info->ps.exports_mrtz_via_epilog) {
      info->ps.writes_mrt0_alpha = gfx_state->ms.alpha_to_coverage_via_mrtz && export_alpha_and_mrtz;
   }

   /* Disable VRS and use the rates from PS_ITER_SAMPLES if:
    *
    * - The fragment shader reads gl_SampleMaskIn because the 16-bit sample coverage mask isn't enough for MSAA8x and
    *   2x2 coarse shading.
    * - On GFX10.3, if the fragment shader requests a fragment interlock execution mode even if the ordered section was
    *   optimized out, to consistently implement fragmentShadingRateWithFragmentShaderInterlock = VK_FALSE.
    */
   info->ps.force_sample_iter_shading_rate =
      (info->ps.reads_sample_mask_in && !info->ps.needs_poly_line_smooth) ||
      (pdev->info.gfx_level == GFX10_3 &&
       (nir->info.fs.sample_interlock_ordered || nir->info.fs.sample_interlock_unordered ||
        nir->info.fs.pixel_interlock_ordered || nir->info.fs.pixel_interlock_unordered));
}

static void
gather_shader_info_rt(const nir_shader *nir, struct radv_shader_info *info)
{
   // TODO: inline push_constants again
   info->loads_dynamic_offsets = true;
   info->loads_push_constants = true;
   info->can_inline_all_push_constants = false;
   info->inline_push_constant_mask = 0;
   info->desc_set_used_mask = -1u;
}

static void
gather_shader_info_cs(struct radv_device *device, const nir_shader *nir, const struct radv_shader_stage_key *stage_key,
                      struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned default_wave_size = pdev->cs_wave_size;
   if (info->cs.uses_rt)
      default_wave_size = pdev->rt_wave_size;

   unsigned local_size = nir->info.workgroup_size[0] * nir->info.workgroup_size[1] * nir->info.workgroup_size[2];

   /* Games don't always request full subgroups when they should, which can cause bugs if cswave32
    * is enabled. Furthermore, if cooperative matrices or subgroup info are used, we can't transparently change
    * the subgroup size.
    */
   const bool require_full_subgroups =
      stage_key->subgroup_require_full || nir->info.cs.has_cooperative_matrix ||
      (default_wave_size == 32 && nir->info.uses_wide_subgroup_intrinsics && local_size % RADV_SUBGROUP_SIZE == 0);

   const unsigned required_subgroup_size = stage_key->subgroup_required_size * 32;

   if (required_subgroup_size) {
      info->wave_size = required_subgroup_size;
   } else if (require_full_subgroups) {
      info->wave_size = RADV_SUBGROUP_SIZE;
   } else if (pdev->info.gfx_level >= GFX10 && local_size <= 32) {
      /* Use wave32 for small workgroups. */
      info->wave_size = 32;
   } else {
      info->wave_size = default_wave_size;
   }

   if (pdev->info.has_cs_regalloc_hang_bug) {
      info->cs.regalloc_hang_bug = info->cs.block_size[0] * info->cs.block_size[1] * info->cs.block_size[2] > 256;
   }
}

static void
gather_shader_info_task(struct radv_device *device, const nir_shader *nir,
                        const struct radv_shader_stage_key *stage_key, struct radv_shader_info *info)
{
   gather_shader_info_cs(device, nir, stage_key, info);

   /* Task shaders always need these for the I/O lowering even if the API shader doesn't actually
    * use them.
    */

   /* Needed to address the task draw/payload rings. */
   info->cs.uses_block_id[0] = true;
   info->cs.uses_block_id[1] = true;
   info->cs.uses_block_id[2] = true;
   info->cs.uses_grid_size = true;

   /* Needed for storing draw ready only on the 1st thread. */
   info->cs.uses_local_invocation_idx = true;

   /* Task->Mesh dispatch is linear when Y = Z = 1.
    * GFX11 CP can optimize this case with a field in its draw packets.
    */
   info->cs.linear_taskmesh_dispatch =
      nir->info.mesh.ts_mesh_dispatch_dimensions[1] == 1 && nir->info.mesh.ts_mesh_dispatch_dimensions[2] == 1;

   info->cs.has_query = device->cache_key.mesh_shader_queries;
}

static uint32_t
radv_get_user_data_0(const struct radv_device *device, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

   switch (info->stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_MESH:
      if (info->next_stage == MESA_SHADER_TESS_CTRL) {
         assert(info->stage == MESA_SHADER_VERTEX);

         if (gfx_level >= GFX10) {
            return R_00B430_SPI_SHADER_USER_DATA_HS_0;
         } else if (gfx_level == GFX9) {
            return R_00B430_SPI_SHADER_USER_DATA_LS_0;
         } else {
            return R_00B530_SPI_SHADER_USER_DATA_LS_0;
         }
      }

      if (info->next_stage == MESA_SHADER_GEOMETRY) {
         assert(info->stage == MESA_SHADER_VERTEX || info->stage == MESA_SHADER_TESS_EVAL);

         if (gfx_level >= GFX10) {
            return R_00B230_SPI_SHADER_USER_DATA_GS_0;
         } else {
            return R_00B330_SPI_SHADER_USER_DATA_ES_0;
         }
      }

      if (info->is_ngg)
         return R_00B230_SPI_SHADER_USER_DATA_GS_0;

      assert(info->stage != MESA_SHADER_MESH);
      return R_00B130_SPI_SHADER_USER_DATA_VS_0;
   case MESA_SHADER_TESS_CTRL:
      return gfx_level == GFX9 ? R_00B430_SPI_SHADER_USER_DATA_LS_0 : R_00B430_SPI_SHADER_USER_DATA_HS_0;
   case MESA_SHADER_GEOMETRY:
      return gfx_level == GFX9 ? R_00B330_SPI_SHADER_USER_DATA_ES_0 : R_00B230_SPI_SHADER_USER_DATA_GS_0;
   case MESA_SHADER_FRAGMENT:
      return R_00B030_SPI_SHADER_USER_DATA_PS_0;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TASK:
   case MESA_SHADER_RAYGEN:
   case MESA_SHADER_CALLABLE:
   case MESA_SHADER_CLOSEST_HIT:
   case MESA_SHADER_MISS:
   case MESA_SHADER_INTERSECTION:
   case MESA_SHADER_ANY_HIT:
      return R_00B900_COMPUTE_USER_DATA_0;
   default:
      unreachable("invalid shader stage");
   }
}

static bool
radv_is_merged_shader_compiled_separately(const struct radv_device *device, const struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

   if (gfx_level >= GFX9) {
      switch (info->stage) {
      case MESA_SHADER_VERTEX:
         if (info->next_stage == MESA_SHADER_TESS_CTRL || info->next_stage == MESA_SHADER_GEOMETRY)
            return !info->outputs_linked;
         break;
      case MESA_SHADER_TESS_EVAL:
         if (info->next_stage == MESA_SHADER_GEOMETRY)
            return !info->outputs_linked;
         break;
      case MESA_SHADER_TESS_CTRL:
      case MESA_SHADER_GEOMETRY:
         return !info->inputs_linked;
      default:
         break;
      }
   }

   return false;
}

void
radv_nir_shader_info_init(gl_shader_stage stage, gl_shader_stage next_stage, struct radv_shader_info *info)
{
   memset(info, 0, sizeof(*info));

   /* Assume that shaders can inline all push constants by default. */
   info->can_inline_all_push_constants = true;

   info->stage = stage;
   info->next_stage = next_stage;
}

void
radv_nir_shader_info_pass(struct radv_device *device, const struct nir_shader *nir,
                          const struct radv_shader_layout *layout, const struct radv_shader_stage_key *stage_key,
                          const struct radv_graphics_state_key *gfx_state, const enum radv_pipeline_type pipeline_type,
                          bool consider_force_vrs, struct radv_shader_info *info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct nir_function *func = (struct nir_function *)exec_list_get_head_const(&nir->functions);

   if (layout->use_dynamic_descriptors) {
      info->loads_push_constants = true;
      info->loads_dynamic_offsets = true;
   }

   nir_foreach_block (block, func->impl) {
      gather_info_block(nir, block, info, gfx_state, stage_key, consider_force_vrs);
   }

   if (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_GEOMETRY)
      gather_xfb_info(nir, info);

   if (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_GEOMETRY || nir->info.stage == MESA_SHADER_MESH) {
      struct radv_vs_output_info *outinfo = &info->outinfo;
      uint64_t per_vtx_mask, per_prim_mask;

      radv_get_output_masks(nir, gfx_state, &per_vtx_mask, &per_prim_mask);

      /* Mesh multiview is only lowered in ac_nir_lower_ngg, so we have to fake it here. */
      if (nir->info.stage == MESA_SHADER_MESH && gfx_state->has_multiview_view_index)
         info->uses_view_index = true;

      /* Per vertex outputs. */
      outinfo->writes_pointsize = per_vtx_mask & VARYING_BIT_PSIZ;
      outinfo->writes_viewport_index = per_vtx_mask & VARYING_BIT_VIEWPORT;
      outinfo->writes_layer = per_vtx_mask & VARYING_BIT_LAYER;
      outinfo->writes_primitive_shading_rate =
         (per_vtx_mask & VARYING_BIT_PRIMITIVE_SHADING_RATE) || info->force_vrs_per_vertex;

      /* Per primitive outputs. */
      outinfo->writes_viewport_index_per_primitive = per_prim_mask & VARYING_BIT_VIEWPORT;
      outinfo->writes_layer_per_primitive = per_prim_mask & VARYING_BIT_LAYER;
      outinfo->writes_primitive_shading_rate_per_primitive = per_prim_mask & VARYING_BIT_PRIMITIVE_SHADING_RATE;

      /* Clip/cull distances. */
      outinfo->clip_dist_mask = (1 << nir->info.clip_distance_array_size) - 1;
      outinfo->cull_dist_mask = (1 << nir->info.cull_distance_array_size) - 1;
      outinfo->cull_dist_mask <<= nir->info.clip_distance_array_size;

      int pos_written = 0x1;

      if (outinfo->writes_pointsize || outinfo->writes_viewport_index || outinfo->writes_layer ||
          outinfo->writes_primitive_shading_rate)
         pos_written |= 1 << 1;

      unsigned num_clip_distances = util_bitcount(outinfo->clip_dist_mask);
      unsigned num_cull_distances = util_bitcount(outinfo->cull_dist_mask);

      if (num_clip_distances + num_cull_distances > 0)
         pos_written |= 1 << 2;
      if (num_clip_distances + num_cull_distances > 4)
         pos_written |= 1 << 3;

      outinfo->pos_exports = util_bitcount(pos_written);
   }

   info->vs.needs_draw_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID);
   info->vs.needs_base_instance |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE);
   info->vs.needs_instance_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID);
   info->uses_view_index |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VIEW_INDEX);
   info->uses_invocation_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INVOCATION_ID);
   info->uses_prim_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);

   /* Used by compute and mesh shaders. Mesh shaders must always declare this before GFX11. */
   info->cs.uses_grid_size = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_WORKGROUPS) ||
                             (nir->info.stage == MESA_SHADER_MESH && pdev->info.gfx_level < GFX11);
   info->cs.uses_local_invocation_idx = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_LOCAL_INVOCATION_INDEX) |
                                        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SUBGROUP_ID) |
                                        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_SUBGROUPS) |
                                        radv_shader_should_clear_lds(device, nir);

   if (nir->info.stage == MESA_SHADER_COMPUTE || nir->info.stage == MESA_SHADER_TASK ||
       nir->info.stage == MESA_SHADER_MESH) {
      for (int i = 0; i < 3; ++i)
         info->cs.block_size[i] = nir->info.workgroup_size[i];
   }

   info->user_data_0 = radv_get_user_data_0(device, info);
   info->merged_shader_compiled_separately = radv_is_merged_shader_compiled_separately(device, info);
   info->force_indirect_desc_sets = info->merged_shader_compiled_separately || stage_key->indirect_bindable;

   switch (nir->info.stage) {
   case MESA_SHADER_COMPUTE:
      gather_shader_info_cs(device, nir, stage_key, info);
      break;
   case MESA_SHADER_TASK:
      gather_shader_info_task(device, nir, stage_key, info);
      break;
   case MESA_SHADER_FRAGMENT:
      gather_shader_info_fs(device, nir, gfx_state, info);
      break;
   case MESA_SHADER_GEOMETRY:
      gather_shader_info_gs(device, nir, info);
      break;
   case MESA_SHADER_TESS_EVAL:
      gather_shader_info_tes(device, nir, info);
      break;
   case MESA_SHADER_TESS_CTRL:
      gather_shader_info_tcs(device, nir, gfx_state, info);
      break;
   case MESA_SHADER_VERTEX:
      gather_shader_info_vs(device, nir, gfx_state, stage_key, info);
      break;
   case MESA_SHADER_MESH:
      gather_shader_info_mesh(device, nir, stage_key, info);
      break;
   default:
      if (gl_shader_stage_is_rt(nir->info.stage))
         gather_shader_info_rt(nir, info);
      break;
   }

   info->wave_size = radv_get_wave_size(device, nir->info.stage, info, stage_key);
   info->ballot_bit_size = radv_get_ballot_bit_size(device, nir->info.stage, info, stage_key);

   switch (nir->info.stage) {
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TASK:
      info->workgroup_size = ac_compute_cs_workgroup_size(nir->info.workgroup_size, false, UINT32_MAX);

      /* Allow the compiler to assume that the shader always has full subgroups,
       * meaning that the initial EXEC mask is -1 in all waves (all lanes enabled).
       * This assumption is incorrect for ray tracing and internal (meta) shaders
       * because they can use unaligned dispatch.
       */
      info->cs.uses_full_subgroups = pipeline_type != RADV_PIPELINE_RAY_TRACING && !nir->info.internal &&
                                     (info->workgroup_size % info->wave_size) == 0;
      break;
   case MESA_SHADER_VERTEX:
      if (info->vs.as_ls || info->vs.as_es) {
         /* Set the maximum possible value by default, this will be optimized during linking if
          * possible.
          */
         info->workgroup_size = 256;
      } else {
         info->workgroup_size = info->wave_size;
      }
      break;
   case MESA_SHADER_TESS_CTRL:
      if (gfx_state->ts.patch_control_points) {
         info->workgroup_size =
            ac_compute_lshs_workgroup_size(pdev->info.gfx_level, MESA_SHADER_TESS_CTRL, info->num_tess_patches,
                                           gfx_state->ts.patch_control_points, info->tcs.tcs_vertices_out);
      } else {
         /* Set the maximum possible value when the workgroup size can't be determined. */
         info->workgroup_size = 256;
      }
      break;
   case MESA_SHADER_TESS_EVAL:
      if (info->tes.as_es) {
         /* Set the maximum possible value by default, this will be optimized during linking if
          * possible.
          */
         info->workgroup_size = 256;
      } else {
         info->workgroup_size = info->wave_size;
      }
      break;
   case MESA_SHADER_GEOMETRY:
      if (!info->is_ngg) {
         unsigned es_verts_per_subgroup = info->gs_ring_info.es_verts_per_subgroup;
         unsigned gs_inst_prims_in_subgroup = info->gs_ring_info.gs_inst_prims_in_subgroup;

         info->workgroup_size = ac_compute_esgs_workgroup_size(pdev->info.gfx_level, info->wave_size,
                                                               es_verts_per_subgroup, gs_inst_prims_in_subgroup);
      } else {
         /* Set the maximum possible value by default, this will be optimized during linking if
          * possible.
          */
         info->workgroup_size = 256;
      }
      break;
   case MESA_SHADER_MESH:
      calc_mesh_workgroup_size(device, nir, info);
      break;
   default:
      /* FS always operates without workgroups. Other stages are computed during linking but assume
       * no workgroups by default.
       */
      info->workgroup_size = info->wave_size;
      break;
   }
}

static void
clamp_gsprims_to_esverts(unsigned *max_gsprims, unsigned max_esverts, unsigned min_verts_per_prim, bool use_adjacency)
{
   unsigned max_reuse = max_esverts - min_verts_per_prim;
   if (use_adjacency)
      max_reuse /= 2;
   *max_gsprims = MIN2(*max_gsprims, 1 + max_reuse);
}

static unsigned
radv_get_num_input_vertices(const struct radv_shader_info *es_info, const struct radv_shader_info *gs_info)
{
   if (gs_info) {
      return gs_info->gs.vertices_in;
   }

   if (es_info->stage == MESA_SHADER_TESS_EVAL) {
      if (es_info->tes.point_mode)
         return 1;
      if (es_info->tes._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return 2;
      return 3;
   }

   return 3;
}

static unsigned
radv_get_pre_rast_input_topology(const struct radv_shader_info *es_info, const struct radv_shader_info *gs_info)
{
   if (gs_info) {
      return gs_info->gs.input_prim;
   }

   if (es_info->stage == MESA_SHADER_TESS_EVAL) {
      if (es_info->tes.point_mode)
         return MESA_PRIM_POINTS;
      if (es_info->tes._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return MESA_PRIM_LINES;
      return MESA_PRIM_TRIANGLES;
   }

   return MESA_PRIM_TRIANGLES;
}

static unsigned
gfx10_get_ngg_scratch_lds_base(const struct radv_device *device, const struct radv_shader_info *es_info,
                               const struct radv_shader_info *gs_info, const struct gfx10_ngg_info *ngg_info)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t scratch_lds_base;

   if (gs_info) {
      const unsigned esgs_ring_lds_bytes = ngg_info->esgs_ring_size;
      const unsigned gs_total_out_vtx_bytes = ngg_info->ngg_emit_size * 4u;

      scratch_lds_base = ALIGN(esgs_ring_lds_bytes + gs_total_out_vtx_bytes, 8u /* for the repacking code */);
   } else {
      const bool uses_instanceid = es_info->vs.needs_instance_id;
      const bool uses_primitive_id = es_info->uses_prim_id;
      const bool streamout_enabled = es_info->so.num_outputs && pdev->use_ngg_streamout;
      const uint32_t num_outputs =
         es_info->stage == MESA_SHADER_VERTEX ? es_info->vs.num_outputs : es_info->tes.num_outputs;
      unsigned pervertex_lds_bytes = ac_ngg_nogs_get_pervertex_lds_size(
         es_info->stage, num_outputs, streamout_enabled, es_info->outinfo.export_prim_id, false, /* user edge flag */
         es_info->has_ngg_culling, uses_instanceid, uses_primitive_id);

      assert(ngg_info->hw_max_esverts <= 256);
      unsigned total_es_lds_bytes = pervertex_lds_bytes * ngg_info->hw_max_esverts;

      scratch_lds_base = ALIGN(total_es_lds_bytes, 8u);
   }

   return scratch_lds_base;
}

void
gfx10_get_ngg_info(const struct radv_device *device, struct radv_shader_info *es_info, struct radv_shader_info *gs_info,
                   struct gfx10_ngg_info *out)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const unsigned max_verts_per_prim = radv_get_num_input_vertices(es_info, gs_info);
   const unsigned min_verts_per_prim = gs_info ? max_verts_per_prim : 1;

   const unsigned gs_num_invocations = gs_info ? MAX2(gs_info->gs.invocations, 1) : 1;

   const unsigned input_prim = radv_get_pre_rast_input_topology(es_info, gs_info);
   const bool uses_adjacency = input_prim == MESA_PRIM_LINES_ADJACENCY || input_prim == MESA_PRIM_TRIANGLES_ADJACENCY;

   /* All these are in dwords: */
   /* We can't allow using the whole LDS, because GS waves compete with
    * other shader stages for LDS space.
    *
    * TODO: We should really take the shader's internal LDS use into
    *       account. The linker will fail if the size is greater than
    *       8K dwords.
    */
   const unsigned max_lds_size = 8 * 1024 - 768;
   const unsigned target_lds_size = max_lds_size;
   unsigned esvert_lds_size = 0;
   unsigned gsprim_lds_size = 0;

   /* All these are per subgroup: */
   const unsigned min_esverts = gfx_level >= GFX11 ? 3 : /* gfx11 requires at least 1 primitive per TG */
                                   gfx_level >= GFX10_3 ? 29
                                                        : 24;
   bool max_vert_out_per_gs_instance = false;
   unsigned max_esverts_base = 128;
   unsigned max_gsprims_base = 128; /* default prim group size clamp */

   /* Hardware has the following non-natural restrictions on the value
    * of GE_CNTL.VERT_GRP_SIZE based on based on the primitive type of
    * the draw:
    *  - at most 252 for any line input primitive type
    *  - at most 251 for any quad input primitive type
    *  - at most 251 for triangle strips with adjacency (this happens to
    *    be the natural limit for triangle *lists* with adjacency)
    */
   max_esverts_base = MIN2(max_esverts_base, 251 + max_verts_per_prim - 1);

   if (gs_info) {
      unsigned max_out_verts_per_gsprim = gs_info->gs.vertices_out * gs_num_invocations;

      if (max_out_verts_per_gsprim <= 256) {
         if (max_out_verts_per_gsprim) {
            max_gsprims_base = MIN2(max_gsprims_base, 256 / max_out_verts_per_gsprim);
         }
      } else {
         /* Use special multi-cycling mode in which each GS
          * instance gets its own subgroup. Does not work with
          * tessellation. */
         max_vert_out_per_gs_instance = true;
         max_gsprims_base = 1;
         max_out_verts_per_gsprim = gs_info->gs.vertices_out;
      }

      esvert_lds_size = es_info->esgs_itemsize / 4;
      gsprim_lds_size = (gs_info->gs.gsvs_vertex_size / 4 + 1) * max_out_verts_per_gsprim;
   } else {
      /* VS and TES. */
      /* LDS size for passing data from GS to ES. */
      struct radv_streamout_info *so_info = &es_info->so;

      if (so_info->num_outputs) {
         /* Compute the same pervertex LDS size as the NGG streamout lowering pass which allocates
          * space for all outputs.
          * TODO: only alloc space for outputs that really need streamout.
          */
         const uint32_t num_outputs =
            es_info->stage == MESA_SHADER_VERTEX ? es_info->vs.num_outputs : es_info->tes.num_outputs;
         esvert_lds_size = 4 * num_outputs + 1;
      }

      /* GS stores Primitive IDs (one DWORD) into LDS at the address
       * corresponding to the ES thread of the provoking vertex. All
       * ES threads load and export PrimitiveID for their thread.
       */
      if (es_info->stage == MESA_SHADER_VERTEX && es_info->outinfo.export_prim_id)
         esvert_lds_size = MAX2(esvert_lds_size, 1);
   }

   unsigned max_gsprims = max_gsprims_base;
   unsigned max_esverts = max_esverts_base;

   if (esvert_lds_size)
      max_esverts = MIN2(max_esverts, target_lds_size / esvert_lds_size);
   if (gsprim_lds_size)
      max_gsprims = MIN2(max_gsprims, target_lds_size / gsprim_lds_size);

   max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
   clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
   assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);

   if (esvert_lds_size || gsprim_lds_size) {
      /* Now that we have a rough proportionality between esverts
       * and gsprims based on the primitive type, scale both of them
       * down simultaneously based on required LDS space.
       *
       * We could be smarter about this if we knew how much vertex
       * reuse to expect.
       */
      unsigned lds_total = max_esverts * esvert_lds_size + max_gsprims * gsprim_lds_size;
      if (lds_total > target_lds_size) {
         max_esverts = max_esverts * target_lds_size / lds_total;
         max_gsprims = max_gsprims * target_lds_size / lds_total;

         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      }
   }

   /* Round up towards full wave sizes for better ALU utilization. */
   if (!max_vert_out_per_gs_instance) {
      unsigned orig_max_esverts;
      unsigned orig_max_gsprims;
      unsigned wavesize;

      if (gs_info) {
         wavesize = gs_info->wave_size;
      } else {
         wavesize = es_info->wave_size;
      }

      do {
         orig_max_esverts = max_esverts;
         orig_max_gsprims = max_gsprims;

         max_esverts = align(max_esverts, wavesize);
         max_esverts = MIN2(max_esverts, max_esverts_base);
         if (esvert_lds_size)
            max_esverts = MIN2(max_esverts, (max_lds_size - max_gsprims * gsprim_lds_size) / esvert_lds_size);
         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);

         /* Hardware restriction: minimum value of max_esverts */
         if (gfx_level == GFX10)
            max_esverts = MAX2(max_esverts, min_esverts - 1 + max_verts_per_prim);
         else
            max_esverts = MAX2(max_esverts, min_esverts);

         max_gsprims = align(max_gsprims, wavesize);
         max_gsprims = MIN2(max_gsprims, max_gsprims_base);
         if (gsprim_lds_size) {
            /* Don't count unusable vertices to the LDS
             * size. Those are vertices above the maximum
             * number of vertices that can occur in the
             * workgroup, which is e.g. max_gsprims * 3
             * for triangles.
             */
            unsigned usable_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
            max_gsprims = MIN2(max_gsprims, (max_lds_size - usable_esverts * esvert_lds_size) / gsprim_lds_size);
         }
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      } while (orig_max_esverts != max_esverts || orig_max_gsprims != max_gsprims);

      /* Verify the restriction. */
      if (gfx_level == GFX10)
         assert(max_esverts >= min_esverts - 1 + max_verts_per_prim);
      else
         assert(max_esverts >= min_esverts);
   } else {
      /* Hardware restriction: minimum value of max_esverts */
      if (gfx_level == GFX10)
         max_esverts = MAX2(max_esverts, min_esverts - 1 + max_verts_per_prim);
      else
         max_esverts = MAX2(max_esverts, min_esverts);
   }

   unsigned max_out_vertices = max_vert_out_per_gs_instance ? gs_info->gs.vertices_out
                               : gs_info ? max_gsprims * gs_num_invocations * gs_info->gs.vertices_out
                                         : max_esverts;
   assert(max_out_vertices <= 256);

   unsigned prim_amp_factor = 1;
   if (gs_info) {
      /* Number of output primitives per GS input primitive after
       * GS instancing. */
      prim_amp_factor = gs_info->gs.vertices_out;
   }

   /* On Gfx10, the GE only checks against the maximum number of ES verts
    * after allocating a full GS primitive. So we need to ensure that
    * whenever this check passes, there is enough space for a full
    * primitive without vertex reuse.
    */
   if (gfx_level == GFX10)
      out->hw_max_esverts = max_esverts - max_verts_per_prim + 1;
   else
      out->hw_max_esverts = max_esverts;

   out->max_gsprims = max_gsprims;
   out->max_out_verts = max_out_vertices;
   out->prim_amp_factor = prim_amp_factor;
   out->max_vert_out_per_gs_instance = max_vert_out_per_gs_instance;
   out->ngg_emit_size = max_gsprims * gsprim_lds_size;

   /* Don't count unusable vertices. */
   out->esgs_ring_size = MIN2(max_esverts, max_gsprims * max_verts_per_prim) * esvert_lds_size * 4;

   if (gs_info) {
      out->vgt_esgs_ring_itemsize = es_info->esgs_itemsize / 4;
   } else {
      out->vgt_esgs_ring_itemsize = 1;
   }

   assert(out->hw_max_esverts >= min_esverts); /* HW limitation */

   out->scratch_lds_base = gfx10_get_ngg_scratch_lds_base(device, es_info, gs_info, out);

   /* Get scratch LDS usage. */
   const struct radv_shader_info *info = gs_info ? gs_info : es_info;
   const unsigned scratch_lds_size = ac_ngg_get_scratch_lds_size(info->stage, info->workgroup_size, info->wave_size,
                                                                 pdev->use_ngg_streamout, info->has_ngg_culling);
   out->lds_size = out->scratch_lds_base + scratch_lds_size;

   unsigned workgroup_size =
      ac_compute_ngg_workgroup_size(max_esverts, max_gsprims * gs_num_invocations, max_out_vertices, prim_amp_factor);
   if (gs_info) {
      gs_info->workgroup_size = workgroup_size;
   }
   es_info->workgroup_size = workgroup_size;
}

static void
radv_determine_ngg_settings(struct radv_device *device, struct radv_shader_stage *es_stage,
                            struct radv_shader_stage *fs_stage, const struct radv_graphics_state_key *gfx_state)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   assert(es_stage->stage == MESA_SHADER_VERTEX || es_stage->stage == MESA_SHADER_TESS_EVAL);
   assert(!fs_stage || fs_stage->stage == MESA_SHADER_FRAGMENT);

   /* NGG culling is implicitly disabled when the FS stage is unknown. */
   uint64_t ps_inputs_read = fs_stage ? fs_stage->nir->info.inputs_read : ~0;

   unsigned num_vertices_per_prim = 0;
   if (es_stage->stage == MESA_SHADER_VERTEX) {
      num_vertices_per_prim = radv_get_num_vertices_per_prim(gfx_state);
   } else if (es_stage->stage == MESA_SHADER_TESS_EVAL) {
      num_vertices_per_prim = es_stage->nir->info.tess.point_mode                                   ? 1
                              : es_stage->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES ? 2
                                                                                                    : 3;
   }

   es_stage->info.has_ngg_culling =
      radv_consider_culling(pdev, es_stage->nir, ps_inputs_read, num_vertices_per_prim, &es_stage->info);

   nir_function_impl *impl = nir_shader_get_entrypoint(es_stage->nir);
   es_stage->info.has_ngg_early_prim_export = exec_list_is_singular(&impl->body);

   /* NGG passthrough mode should be disabled when culling and when the vertex shader
    * exports the primitive ID.
    */
   es_stage->info.is_ngg_passthrough = !es_stage->info.has_ngg_culling && !(es_stage->stage == MESA_SHADER_VERTEX &&
                                                                            es_stage->info.outinfo.export_prim_id);
}

static void
radv_link_shaders_info(struct radv_device *device, struct radv_shader_stage *producer,
                       struct radv_shader_stage *consumer, const struct radv_graphics_state_key *gfx_state)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* Export primitive ID and clip/cull distances if read by the FS, or export unconditionally when
    * the next stage is unknown (with graphics pipeline library).
    */
   if (producer->info.next_stage == MESA_SHADER_FRAGMENT ||
       !(gfx_state->lib_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT)) {
      const bool ps_prim_id_in = !consumer || consumer->info.ps.prim_id_input;
      const bool ps_clip_dists_in = !consumer || !!consumer->info.ps.input_clips_culls_mask;

      radv_set_vs_output_param(device, producer->nir, gfx_state, &producer->info, ps_prim_id_in, ps_clip_dists_in);
   }

   if (producer->stage == MESA_SHADER_VERTEX || producer->stage == MESA_SHADER_TESS_EVAL) {
      /* Compute NGG info (GFX10+) or GS info. */
      if (producer->info.is_ngg) {
         struct radv_shader_stage *gs_stage = consumer && consumer->stage == MESA_SHADER_GEOMETRY ? consumer : NULL;
         struct gfx10_ngg_info *out = gs_stage ? &gs_stage->info.ngg_info : &producer->info.ngg_info;

         /* Determine other NGG settings like culling for VS or TES without GS. */
         if (!gs_stage) {
            radv_determine_ngg_settings(device, producer, consumer, gfx_state);
         }

         gfx10_get_ngg_info(device, &producer->info, gs_stage ? &gs_stage->info : NULL, out);
      } else if (consumer && consumer->stage == MESA_SHADER_GEOMETRY) {
         struct radv_shader_info *gs_info = &consumer->info;
         struct radv_shader_info *es_info = &producer->info;

         es_info->workgroup_size = gs_info->workgroup_size;
      }
   }

   if (producer->stage == MESA_SHADER_VERTEX && consumer && consumer->stage == MESA_SHADER_TESS_CTRL) {
      struct radv_shader_stage *vs_stage = producer;
      struct radv_shader_stage *tcs_stage = consumer;

      if (gfx_state->ts.patch_control_points) {
         vs_stage->info.workgroup_size =
            ac_compute_lshs_workgroup_size(pdev->info.gfx_level, MESA_SHADER_VERTEX, tcs_stage->info.num_tess_patches,
                                           gfx_state->ts.patch_control_points, tcs_stage->info.tcs.tcs_vertices_out);

         if (!radv_use_llvm_for_stage(pdev, MESA_SHADER_VERTEX)) {
            /* When the number of TCS input and output vertices are the same (typically 3):
             * - There is an equal amount of LS and HS invocations
             * - In case of merged LSHS shaders, the LS and HS halves of the shader always process
             *   the exact same vertex. We can use this knowledge to optimize them.
             *
             * We don't set tcs_in_out_eq if the float controls differ because that might involve
             * different float modes for the same block and our optimizer doesn't handle a
             * instruction dominating another with a different mode.
             */
            vs_stage->info.vs.tcs_in_out_eq =
               pdev->info.gfx_level >= GFX9 &&
               gfx_state->ts.patch_control_points == tcs_stage->info.tcs.tcs_vertices_out &&
               vs_stage->nir->info.float_controls_execution_mode == tcs_stage->nir->info.float_controls_execution_mode;

            if (vs_stage->info.vs.tcs_in_out_eq)
               vs_stage->info.vs.tcs_temp_only_input_mask =
                  tcs_stage->nir->info.inputs_read & vs_stage->nir->info.outputs_written &
                  ~tcs_stage->nir->info.tess.tcs_cross_invocation_inputs_read &
                  ~tcs_stage->nir->info.inputs_read_indirectly & ~vs_stage->nir->info.outputs_accessed_indirectly;
         }
      }
   }

   /* Copy shader info between TCS<->TES. */
   if (producer->stage == MESA_SHADER_TESS_CTRL && consumer && consumer->stage == MESA_SHADER_TESS_EVAL) {
      struct radv_shader_stage *tcs_stage = producer;
      struct radv_shader_stage *tes_stage = consumer;

      tcs_stage->info.tcs.tes_reads_tess_factors = tes_stage->info.tes.reads_tess_factors;
      tcs_stage->info.tcs.tes_inputs_read = tes_stage->nir->info.inputs_read;
      tcs_stage->info.tcs.tes_patch_inputs_read = tes_stage->nir->info.patch_inputs_read;
      tcs_stage->info.tes._primitive_mode = tes_stage->nir->info.tess._primitive_mode;

      if (gfx_state->ts.patch_control_points)
         tes_stage->info.num_tess_patches = tcs_stage->info.num_tess_patches;
   }
}

static void
radv_nir_shader_info_merge(const struct radv_shader_stage *src, struct radv_shader_stage *dst)
{
   const struct radv_shader_info *src_info = &src->info;
   struct radv_shader_info *dst_info = &dst->info;

   assert((src->stage == MESA_SHADER_VERTEX && dst->stage == MESA_SHADER_TESS_CTRL) ||
          (src->stage == MESA_SHADER_VERTEX && dst->stage == MESA_SHADER_GEOMETRY) ||
          (src->stage == MESA_SHADER_TESS_EVAL && dst->stage == MESA_SHADER_GEOMETRY));

   dst_info->loads_push_constants |= src_info->loads_push_constants;
   dst_info->loads_dynamic_offsets |= src_info->loads_dynamic_offsets;
   dst_info->desc_set_used_mask |= src_info->desc_set_used_mask;
   dst_info->uses_view_index |= src_info->uses_view_index;
   dst_info->uses_prim_id |= src_info->uses_prim_id;
   dst_info->inline_push_constant_mask |= src_info->inline_push_constant_mask;

   /* Only inline all push constants if both allows it. */
   dst_info->can_inline_all_push_constants &= src_info->can_inline_all_push_constants;

   if (src->stage == MESA_SHADER_VERTEX) {
      dst_info->vs = src_info->vs;
   } else {
      dst_info->tes = src_info->tes;
   }

   if (dst->stage == MESA_SHADER_GEOMETRY)
      dst_info->gs.es_type = src->stage;
}

static const gl_shader_stage graphics_shader_order[] = {
   MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL, MESA_SHADER_GEOMETRY,

   MESA_SHADER_TASK,   MESA_SHADER_MESH,
};

void
radv_nir_shader_info_link(struct radv_device *device, const struct radv_graphics_state_key *gfx_state,
                          struct radv_shader_stage *stages)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* Walk backwards to link */
   struct radv_shader_stage *next_stage = stages[MESA_SHADER_FRAGMENT].nir ? &stages[MESA_SHADER_FRAGMENT] : NULL;

   for (int i = ARRAY_SIZE(graphics_shader_order) - 1; i >= 0; i--) {
      gl_shader_stage s = graphics_shader_order[i];
      if (!stages[s].nir)
         continue;

      radv_link_shaders_info(device, &stages[s], next_stage, gfx_state);
      next_stage = &stages[s];
   }

   if (pdev->info.gfx_level >= GFX9) {
      /* Merge shader info for VS+TCS. */
      if (stages[MESA_SHADER_VERTEX].nir && stages[MESA_SHADER_TESS_CTRL].nir) {
         radv_nir_shader_info_merge(&stages[MESA_SHADER_VERTEX], &stages[MESA_SHADER_TESS_CTRL]);
      }

      /* Merge shader info for VS+GS or TES+GS. */
      if ((stages[MESA_SHADER_VERTEX].nir || stages[MESA_SHADER_TESS_EVAL].nir) && stages[MESA_SHADER_GEOMETRY].nir) {
         gl_shader_stage pre_stage = stages[MESA_SHADER_TESS_EVAL].nir ? MESA_SHADER_TESS_EVAL : MESA_SHADER_VERTEX;

         radv_nir_shader_info_merge(&stages[pre_stage], &stages[MESA_SHADER_GEOMETRY]);
      }
   }
}

enum ac_hw_stage
radv_select_hw_stage(const struct radv_shader_info *const info, const enum amd_gfx_level gfx_level)
{
   switch (info->stage) {
   case MESA_SHADER_VERTEX:
      if (info->is_ngg)
         return AC_HW_NEXT_GEN_GEOMETRY_SHADER;
      else if (info->vs.as_es)
         return gfx_level >= GFX9 ? AC_HW_LEGACY_GEOMETRY_SHADER : AC_HW_EXPORT_SHADER;
      else if (info->vs.as_ls)
         return gfx_level >= GFX9 ? AC_HW_HULL_SHADER : AC_HW_LOCAL_SHADER;
      else
         return AC_HW_VERTEX_SHADER;
   case MESA_SHADER_TESS_EVAL:
      if (info->is_ngg)
         return AC_HW_NEXT_GEN_GEOMETRY_SHADER;
      else if (info->tes.as_es)
         return gfx_level >= GFX9 ? AC_HW_LEGACY_GEOMETRY_SHADER : AC_HW_EXPORT_SHADER;
      else
         return AC_HW_VERTEX_SHADER;
   case MESA_SHADER_TESS_CTRL:
      return AC_HW_HULL_SHADER;
   case MESA_SHADER_GEOMETRY:
      if (info->is_ngg)
         return AC_HW_NEXT_GEN_GEOMETRY_SHADER;
      else
         return AC_HW_LEGACY_GEOMETRY_SHADER;
   case MESA_SHADER_MESH:
      return AC_HW_NEXT_GEN_GEOMETRY_SHADER;
   case MESA_SHADER_FRAGMENT:
      return AC_HW_PIXEL_SHADER;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
   case MESA_SHADER_TASK:
   case MESA_SHADER_RAYGEN:
   case MESA_SHADER_ANY_HIT:
   case MESA_SHADER_CLOSEST_HIT:
   case MESA_SHADER_MISS:
   case MESA_SHADER_INTERSECTION:
   case MESA_SHADER_CALLABLE:
      return AC_HW_COMPUTE_SHADER;
   default:
      unreachable("Unsupported HW stage");
   }
}

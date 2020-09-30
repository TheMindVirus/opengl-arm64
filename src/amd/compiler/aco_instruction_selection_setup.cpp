/*
 * Copyright © 2018 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <array>
#include <unordered_map>
#include "aco_ir.h"
#include "nir.h"
#include "nir_control_flow.h"
#include "vulkan/radv_shader.h"
#include "vulkan/radv_descriptor_set.h"
#include "vulkan/radv_shader_args.h"
#include "sid.h"
#include "ac_exp_param.h"
#include "ac_shader_util.h"

#include "util/u_math.h"

#define MAX_INLINE_PUSH_CONSTS 8

namespace aco {

struct shader_io_state {
   uint8_t mask[VARYING_SLOT_MAX];
   Temp temps[VARYING_SLOT_MAX * 4u];

   shader_io_state() {
      memset(mask, 0, sizeof(mask));
      std::fill_n(temps, VARYING_SLOT_MAX * 4u, Temp(0, RegClass::v1));
   }
};

enum resource_flags {
   has_glc_vmem_load = 0x1,
   has_nonglc_vmem_load = 0x2,
   has_glc_vmem_store = 0x4,
   has_nonglc_vmem_store = 0x8,

   has_vmem_store = has_glc_vmem_store | has_nonglc_vmem_store,
   has_vmem_loadstore = has_vmem_store | has_glc_vmem_load | has_nonglc_vmem_load,
   has_nonglc_vmem_loadstore = has_nonglc_vmem_load | has_nonglc_vmem_store,

   buffer_is_restrict = 0x10,
};

struct isel_context {
   const struct radv_nir_compiler_options *options;
   struct radv_shader_args *args;
   Program *program;
   nir_shader *shader;
   uint32_t constant_data_offset;
   Block *block;
   std::unique_ptr<Temp[]> allocated;
   std::unordered_map<unsigned, std::array<Temp,NIR_MAX_VEC_COMPONENTS>> allocated_vec;
   Stage stage; /* Stage */
   bool has_gfx10_wave64_bpermute = false;
   struct {
      bool has_branch;
      uint16_t loop_nest_depth = 0;
      struct {
         unsigned header_idx;
         Block* exit;
         bool has_divergent_continue = false;
         bool has_divergent_branch = false;
      } parent_loop;
      struct {
         bool is_divergent = false;
      } parent_if;
      bool exec_potentially_empty_discard = false; /* set to false when loop_nest_depth==0 && parent_if.is_divergent==false */
      uint16_t exec_potentially_empty_break_depth = UINT16_MAX;
      /* Set to false when loop_nest_depth==exec_potentially_empty_break_depth
       * and parent_if.is_divergent==false. Called _break but it's also used for
       * loop continues. */
      bool exec_potentially_empty_break = false;
      std::unique_ptr<unsigned[]> nir_to_aco; /* NIR block index to ACO block index */
   } cf_info;

   uint32_t resource_flag_offsets[MAX_SETS];
   std::vector<uint8_t> buffer_resource_flags;

   Temp arg_temps[AC_MAX_ARGS];

   /* FS inputs */
   Temp persp_centroid, linear_centroid;

   /* GS inputs */
   Temp gs_wave_id;

   /* VS output information */
   bool export_clip_dists;
   unsigned num_clip_distances;
   unsigned num_cull_distances;

   /* tessellation information */
   unsigned tcs_tess_lvl_out_loc;
   unsigned tcs_tess_lvl_in_loc;
   uint64_t tcs_temp_only_inputs;
   uint32_t tcs_num_inputs;
   uint32_t tcs_num_outputs;
   uint32_t tcs_num_patch_outputs;
   uint32_t tcs_num_patches;
   bool tcs_in_out_eq = false;

   /* I/O information */
   shader_io_state inputs;
   shader_io_state outputs;
   uint8_t output_drv_loc_to_var_slot[MESA_SHADER_COMPUTE][VARYING_SLOT_MAX];
   uint8_t output_tcs_patch_drv_loc_to_var_slot[VARYING_SLOT_MAX];
};

Temp get_arg(isel_context *ctx, struct ac_arg arg)
{
   assert(arg.used);
   return ctx->arg_temps[arg.arg_index];
}

unsigned get_interp_input(nir_intrinsic_op intrin, enum glsl_interp_mode interp)
{
   switch (interp) {
   case INTERP_MODE_SMOOTH:
   case INTERP_MODE_NONE:
      if (intrin == nir_intrinsic_load_barycentric_pixel ||
          intrin == nir_intrinsic_load_barycentric_at_sample ||
          intrin == nir_intrinsic_load_barycentric_at_offset)
         return S_0286CC_PERSP_CENTER_ENA(1);
      else if (intrin == nir_intrinsic_load_barycentric_centroid)
         return S_0286CC_PERSP_CENTROID_ENA(1);
      else if (intrin == nir_intrinsic_load_barycentric_sample)
         return S_0286CC_PERSP_SAMPLE_ENA(1);
      break;
   case INTERP_MODE_NOPERSPECTIVE:
      if (intrin == nir_intrinsic_load_barycentric_pixel)
         return S_0286CC_LINEAR_CENTER_ENA(1);
      else if (intrin == nir_intrinsic_load_barycentric_centroid)
         return S_0286CC_LINEAR_CENTROID_ENA(1);
      else if (intrin == nir_intrinsic_load_barycentric_sample)
         return S_0286CC_LINEAR_SAMPLE_ENA(1);
      break;
   default:
      break;
   }
   return 0;
}

/* If one side of a divergent IF ends in a branch and the other doesn't, we
 * might have to emit the contents of the side without the branch at the merge
 * block instead. This is so that we can use any SGPR live-out of the side
 * without the branch without creating a linear phi in the invert or merge block. */
bool
sanitize_if(nir_function_impl *impl, nir_if *nif)
{
   //TODO: skip this if the condition is uniform and there are no divergent breaks/continues?

   nir_block *then_block = nir_if_last_then_block(nif);
   nir_block *else_block = nir_if_last_else_block(nif);
   bool then_jump = nir_block_ends_in_jump(then_block) || nir_block_is_unreachable(then_block);
   bool else_jump = nir_block_ends_in_jump(else_block) || nir_block_is_unreachable(else_block);
   if (then_jump == else_jump)
      return false;

   /* If the continue from block is empty then return as there is nothing to
    * move.
    */
   if (nir_cf_list_is_empty_block(else_jump ? &nif->then_list : &nif->else_list))
      return false;

   /* Even though this if statement has a jump on one side, we may still have
    * phis afterwards.  Single-source phis can be produced by loop unrolling
    * or dead control-flow passes and are perfectly legal.  Run a quick phi
    * removal on the block after the if to clean up any such phis.
    */
   nir_opt_remove_phis_block(nir_cf_node_as_block(nir_cf_node_next(&nif->cf_node)));

   /* Finally, move the continue from branch after the if-statement. */
   nir_block *last_continue_from_blk = else_jump ? then_block : else_block;
   nir_block *first_continue_from_blk = else_jump ?
      nir_if_first_then_block(nif) : nir_if_first_else_block(nif);

   nir_cf_list tmp;
   nir_cf_extract(&tmp, nir_before_block(first_continue_from_blk),
                        nir_after_block(last_continue_from_blk));
   nir_cf_reinsert(&tmp, nir_after_cf_node(&nif->cf_node));

   /* nir_cf_extract() invalidates dominance metadata, but it should still be
    * correct because of the specific type of transformation we did. Block
    * indices are not valid except for block_0's, which is all we care about for
    * nir_block_is_unreachable(). */
   impl->valid_metadata =
      (nir_metadata)(impl->valid_metadata | nir_metadata_dominance | nir_metadata_block_index);

   return true;
}

bool
sanitize_cf_list(nir_function_impl *impl, struct exec_list *cf_list)
{
   bool progress = false;
   foreach_list_typed(nir_cf_node, cf_node, node, cf_list) {
      switch (cf_node->type) {
      case nir_cf_node_block:
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(cf_node);
         progress |= sanitize_cf_list(impl, &nif->then_list);
         progress |= sanitize_cf_list(impl, &nif->else_list);
         progress |= sanitize_if(impl, nif);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(cf_node);
         progress |= sanitize_cf_list(impl, &loop->body);
         break;
      }
      case nir_cf_node_function:
         unreachable("Invalid cf type");
      }
   }

   return progress;
}

void get_buffer_resource_flags(isel_context *ctx, nir_ssa_def *def, unsigned access,
                               uint8_t **flags, uint32_t *count)
{
   int desc_set = -1;
   unsigned binding = 0;

   if (!def) {
      /* global resources are considered aliasing with all other buffers and
       * buffer images */
      // TODO: only merge flags of resources which can really alias.
   } else if (def->parent_instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(def->parent_instr);
      if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index) {
         desc_set = nir_intrinsic_desc_set(intrin);
         binding = nir_intrinsic_binding(intrin);
      }
   } else if (def->parent_instr->type == nir_instr_type_deref) {
      nir_deref_instr *deref = nir_instr_as_deref(def->parent_instr);
      assert(deref->type->is_image());
      if (deref->type->sampler_dimensionality != GLSL_SAMPLER_DIM_BUF) {
         *flags = NULL;
         *count = 0;
         return;
      }

      nir_variable *var = nir_deref_instr_get_variable(deref);
      desc_set = var->data.descriptor_set;
      binding = var->data.binding;
   }

   if (desc_set < 0) {
      *flags = ctx->buffer_resource_flags.data();
      *count = ctx->buffer_resource_flags.size();
      return;
   }

   unsigned set_offset = ctx->resource_flag_offsets[desc_set];

   if (!(ctx->buffer_resource_flags[set_offset + binding] & buffer_is_restrict)) {
      /* Non-restrict buffers alias only with other non-restrict buffers.
       * We reserve flags[0] for these. */
      *flags = ctx->buffer_resource_flags.data();
      *count = 1;
      return;
   }

   *flags = ctx->buffer_resource_flags.data() + set_offset + binding;
   *count = 1;
}

uint8_t get_all_buffer_resource_flags(isel_context *ctx, nir_ssa_def *def, unsigned access)
{
   uint8_t *flags;
   uint32_t count;
   get_buffer_resource_flags(ctx, def, access, &flags, &count);

   uint8_t res = 0;
   for (unsigned i = 0; i < count; i++)
      res |= flags[i];
   return res;
}

bool can_subdword_ssbo_store_use_smem(nir_intrinsic_instr *intrin)
{
   unsigned wrmask = nir_intrinsic_write_mask(intrin);
   if (util_last_bit(wrmask) != util_bitcount(wrmask) ||
       util_bitcount(wrmask) * intrin->src[0].ssa->bit_size % 32 ||
       util_bitcount(wrmask) != intrin->src[0].ssa->num_components)
      return false;

   if (nir_intrinsic_align_mul(intrin) % 4 || nir_intrinsic_align_offset(intrin) % 4)
      return false;

   return true;
}

void fill_desc_set_info(isel_context *ctx, nir_function_impl *impl)
{
   radv_pipeline_layout *pipeline_layout = ctx->options->layout;

   unsigned resource_flag_count = 1; /* +1 to reserve flags[0] for aliased resources */
   for (unsigned i = 0; i < pipeline_layout->num_sets; i++) {
      radv_descriptor_set_layout *layout = pipeline_layout->set[i].layout;
      ctx->resource_flag_offsets[i] = resource_flag_count;
      resource_flag_count += layout->binding_count;
   }
   ctx->buffer_resource_flags = std::vector<uint8_t>(resource_flag_count);

   nir_foreach_variable_with_modes(var, impl->function->shader, nir_var_mem_ssbo) {
      if (var->data.access & ACCESS_RESTRICT) {
         uint32_t offset = ctx->resource_flag_offsets[var->data.descriptor_set];
         ctx->buffer_resource_flags[offset + var->data.binding] |= buffer_is_restrict;
      }
   }

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (!(nir_intrinsic_infos[intrin->intrinsic].index_map[NIR_INTRINSIC_ACCESS]))
            continue;

         nir_ssa_def *res = NULL;
         unsigned access = nir_intrinsic_access(intrin);
         unsigned flags = 0;
         bool glc = access & (ACCESS_VOLATILE | ACCESS_COHERENT | ACCESS_NON_READABLE);
         switch (intrin->intrinsic) {
         case nir_intrinsic_load_ssbo: {
            if (nir_dest_is_divergent(intrin->dest) && (!glc || ctx->program->chip_class >= GFX8))
               flags |= glc ? has_glc_vmem_load : has_nonglc_vmem_load;
            res = intrin->src[0].ssa;
            break;
         }
         case nir_intrinsic_ssbo_atomic_add:
         case nir_intrinsic_ssbo_atomic_imin:
         case nir_intrinsic_ssbo_atomic_umin:
         case nir_intrinsic_ssbo_atomic_imax:
         case nir_intrinsic_ssbo_atomic_umax:
         case nir_intrinsic_ssbo_atomic_and:
         case nir_intrinsic_ssbo_atomic_or:
         case nir_intrinsic_ssbo_atomic_xor:
         case nir_intrinsic_ssbo_atomic_exchange:
         case nir_intrinsic_ssbo_atomic_comp_swap:
            flags |= has_glc_vmem_load | has_glc_vmem_store;
            res = intrin->src[0].ssa;
            break;
         case nir_intrinsic_store_ssbo:
            if (nir_src_is_divergent(intrin->src[2]) ||
                ctx->program->chip_class < GFX8 || ctx->program->chip_class >= GFX10_3 ||
                (intrin->src[0].ssa->bit_size < 32 && !can_subdword_ssbo_store_use_smem(intrin)))
               flags |= glc ? has_glc_vmem_store : has_nonglc_vmem_store;
            res = intrin->src[1].ssa;
            break;
         case nir_intrinsic_load_global:
            if (!(access & ACCESS_NON_WRITEABLE))
               flags |= glc ? has_glc_vmem_load : has_nonglc_vmem_load;
            break;
         case nir_intrinsic_store_global:
            flags |= glc ? has_glc_vmem_store : has_nonglc_vmem_store;
            break;
         case nir_intrinsic_global_atomic_add:
         case nir_intrinsic_global_atomic_imin:
         case nir_intrinsic_global_atomic_umin:
         case nir_intrinsic_global_atomic_imax:
         case nir_intrinsic_global_atomic_umax:
         case nir_intrinsic_global_atomic_and:
         case nir_intrinsic_global_atomic_or:
         case nir_intrinsic_global_atomic_xor:
         case nir_intrinsic_global_atomic_exchange:
         case nir_intrinsic_global_atomic_comp_swap:
            flags |= has_glc_vmem_load | has_glc_vmem_store;
            break;
         case nir_intrinsic_image_deref_load:
            res = intrin->src[0].ssa;
            flags |= glc ? has_glc_vmem_load : has_nonglc_vmem_load;
            break;
         case nir_intrinsic_image_deref_store:
            res = intrin->src[0].ssa;
            flags |= (glc || ctx->program->chip_class == GFX6) ? has_glc_vmem_store : has_nonglc_vmem_store;
            break;
         case nir_intrinsic_image_deref_atomic_add:
         case nir_intrinsic_image_deref_atomic_umin:
         case nir_intrinsic_image_deref_atomic_imin:
         case nir_intrinsic_image_deref_atomic_umax:
         case nir_intrinsic_image_deref_atomic_imax:
         case nir_intrinsic_image_deref_atomic_and:
         case nir_intrinsic_image_deref_atomic_or:
         case nir_intrinsic_image_deref_atomic_xor:
         case nir_intrinsic_image_deref_atomic_exchange:
         case nir_intrinsic_image_deref_atomic_comp_swap:
            res = intrin->src[0].ssa;
            flags |= has_glc_vmem_load | has_glc_vmem_store;
            break;
         default:
            continue;
         }

         uint8_t *flags_ptr;
         uint32_t count;
         get_buffer_resource_flags(ctx, res, access, &flags_ptr, &count);

         for (unsigned i = 0; i < count; i++)
            flags_ptr[i] |= flags;
      }
   }
}

void apply_nuw_to_ssa(nir_shader *shader, struct hash_table *range_ht, nir_ssa_def *ssa,
                      const nir_unsigned_upper_bound_config *config)
{
   nir_ssa_scalar scalar;
   scalar.def = ssa;
   scalar.comp = 0;

   if (!nir_ssa_scalar_is_alu(scalar) || nir_ssa_scalar_alu_op(scalar) != nir_op_iadd)
      return;

   nir_alu_instr *add = nir_instr_as_alu(ssa->parent_instr);

   if (add->no_unsigned_wrap)
      return;

   nir_ssa_scalar src0 = nir_ssa_scalar_chase_alu_src(scalar, 0);
   nir_ssa_scalar src1 = nir_ssa_scalar_chase_alu_src(scalar, 1);

   if (nir_ssa_scalar_is_const(src0)) {
      nir_ssa_scalar tmp = src0;
      src0 = src1;
      src1 = tmp;
   }

   uint32_t src1_ub = nir_unsigned_upper_bound(shader, range_ht, src1, config);
   add->no_unsigned_wrap = !nir_addition_might_overflow(shader, range_ht, src0, src1_ub, config);
}

void apply_nuw_to_offsets(isel_context *ctx, nir_function_impl *impl)
{
   nir_unsigned_upper_bound_config config;
   config.min_subgroup_size = 64;
   config.max_subgroup_size = 64;
   if (ctx->shader->info.stage == MESA_SHADER_COMPUTE && ctx->options->key.cs.subgroup_size) {
      config.min_subgroup_size = ctx->options->key.cs.subgroup_size;
      config.max_subgroup_size = ctx->options->key.cs.subgroup_size;
   }
   config.max_work_group_invocations = 2048;
   config.max_work_group_count[0] = 65535;
   config.max_work_group_count[1] = 65535;
   config.max_work_group_count[2] = 65535;
   config.max_work_group_size[0] = 2048;
   config.max_work_group_size[1] = 2048;
   config.max_work_group_size[2] = 2048;
   for (unsigned i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
      unsigned attrib_format = ctx->options->key.vs.vertex_attribute_formats[i];
      unsigned dfmt = attrib_format & 0xf;
      unsigned nfmt = (attrib_format >> 4) & 0x7;

      uint32_t max = UINT32_MAX;
      if (nfmt == V_008F0C_BUF_NUM_FORMAT_UNORM) {
         max = 0x3f800000u;
      } else if (nfmt == V_008F0C_BUF_NUM_FORMAT_UINT ||
                 nfmt == V_008F0C_BUF_NUM_FORMAT_USCALED) {
         bool uscaled = nfmt == V_008F0C_BUF_NUM_FORMAT_USCALED;
         switch (dfmt) {
         case V_008F0C_BUF_DATA_FORMAT_8:
         case V_008F0C_BUF_DATA_FORMAT_8_8:
         case V_008F0C_BUF_DATA_FORMAT_8_8_8_8:
            max = uscaled ? 0x437f0000u : UINT8_MAX;
            break;
         case V_008F0C_BUF_DATA_FORMAT_10_10_10_2:
         case V_008F0C_BUF_DATA_FORMAT_2_10_10_10:
            max = uscaled ? 0x447fc000u : 1023;
            break;
         case V_008F0C_BUF_DATA_FORMAT_10_11_11:
         case V_008F0C_BUF_DATA_FORMAT_11_11_10:
            max = uscaled ? 0x44ffe000u : 2047;
            break;
         case V_008F0C_BUF_DATA_FORMAT_16:
         case V_008F0C_BUF_DATA_FORMAT_16_16:
         case V_008F0C_BUF_DATA_FORMAT_16_16_16_16:
            max = uscaled ? 0x477fff00u : UINT16_MAX;
            break;
         case V_008F0C_BUF_DATA_FORMAT_32:
         case V_008F0C_BUF_DATA_FORMAT_32_32:
         case V_008F0C_BUF_DATA_FORMAT_32_32_32:
         case V_008F0C_BUF_DATA_FORMAT_32_32_32_32:
            max = uscaled ? 0x4f800000u : UINT32_MAX;
            break;
         }
      }
      config.vertex_attrib_max[i] = max;
   }

   struct hash_table *range_ht = _mesa_pointer_hash_table_create(NULL);

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         switch (intrin->intrinsic) {
         case nir_intrinsic_load_constant:
         case nir_intrinsic_load_uniform:
         case nir_intrinsic_load_push_constant:
            if (!nir_src_is_divergent(intrin->src[0]))
               apply_nuw_to_ssa(ctx->shader, range_ht, intrin->src[0].ssa, &config);
            break;
         case nir_intrinsic_load_ubo:
         case nir_intrinsic_load_ssbo:
            if (!nir_src_is_divergent(intrin->src[1]))
               apply_nuw_to_ssa(ctx->shader, range_ht, intrin->src[1].ssa, &config);
            break;
         case nir_intrinsic_store_ssbo:
            if (!nir_src_is_divergent(intrin->src[2]))
               apply_nuw_to_ssa(ctx->shader, range_ht, intrin->src[2].ssa, &config);
            break;
         default:
            break;
         }
      }
   }

   _mesa_hash_table_destroy(range_ht, NULL);
}

RegClass get_reg_class(isel_context *ctx, RegType type, unsigned components, unsigned bitsize)
{
   if (bitsize == 1)
      return RegClass(RegType::sgpr, ctx->program->lane_mask.size() * components);
   else
      return RegClass::get(type, components * bitsize / 8u);
}

void init_context(isel_context *ctx, nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   unsigned lane_mask_size = ctx->program->lane_mask.size();

   ctx->shader = shader;
   nir_divergence_analysis(shader, nir_divergence_view_index_uniform);

   fill_desc_set_info(ctx, impl);

   apply_nuw_to_offsets(ctx, impl);

   /* sanitize control flow */
   nir_metadata_require(impl, nir_metadata_dominance);
   sanitize_cf_list(impl, &impl->body);
   nir_metadata_preserve(impl, (nir_metadata)~nir_metadata_block_index);

   /* we'll need this for isel */
   nir_metadata_require(impl, nir_metadata_block_index);

   if (!(ctx->stage & sw_gs_copy) && ctx->options->dump_preoptir) {
      fprintf(stderr, "NIR shader before instruction selection:\n");
      nir_print_shader(shader, stderr);
   }

   std::unique_ptr<Temp[]> allocated{new Temp[impl->ssa_alloc]()};

   unsigned spi_ps_inputs = 0;

   std::unique_ptr<unsigned[]> nir_to_aco{new unsigned[impl->num_blocks]()};

   /* TODO: make this recursive to improve compile times and merge with fill_desc_set_info() */
   bool done = false;
   while (!done) {
      done = true;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch(instr->type) {
            case nir_instr_type_alu: {
               nir_alu_instr *alu_instr = nir_instr_as_alu(instr);
               RegType type = RegType::sgpr;
               switch(alu_instr->op) {
                  case nir_op_fmul:
                  case nir_op_fadd:
                  case nir_op_fsub:
                  case nir_op_fmax:
                  case nir_op_fmin:
                  case nir_op_fmax3:
                  case nir_op_fmin3:
                  case nir_op_fmed3:
                  case nir_op_fneg:
                  case nir_op_fabs:
                  case nir_op_fsat:
                  case nir_op_fsign:
                  case nir_op_frcp:
                  case nir_op_frsq:
                  case nir_op_fsqrt:
                  case nir_op_fexp2:
                  case nir_op_flog2:
                  case nir_op_ffract:
                  case nir_op_ffloor:
                  case nir_op_fceil:
                  case nir_op_ftrunc:
                  case nir_op_fround_even:
                  case nir_op_fsin:
                  case nir_op_fcos:
                  case nir_op_f2f16:
                  case nir_op_f2f16_rtz:
                  case nir_op_f2f16_rtne:
                  case nir_op_f2f32:
                  case nir_op_f2f64:
                  case nir_op_u2f16:
                  case nir_op_u2f32:
                  case nir_op_u2f64:
                  case nir_op_i2f16:
                  case nir_op_i2f32:
                  case nir_op_i2f64:
                  case nir_op_pack_half_2x16:
                  case nir_op_unpack_half_2x16_split_x:
                  case nir_op_unpack_half_2x16_split_y:
                  case nir_op_fddx:
                  case nir_op_fddy:
                  case nir_op_fddx_fine:
                  case nir_op_fddy_fine:
                  case nir_op_fddx_coarse:
                  case nir_op_fddy_coarse:
                  case nir_op_fquantize2f16:
                  case nir_op_ldexp:
                  case nir_op_frexp_sig:
                  case nir_op_frexp_exp:
                  case nir_op_cube_face_index:
                  case nir_op_cube_face_coord:
                     type = RegType::vgpr;
                     break;
                  case nir_op_f2i16:
                  case nir_op_f2u16:
                  case nir_op_f2i32:
                  case nir_op_f2u32:
                  case nir_op_f2i64:
                  case nir_op_f2u64:
                  case nir_op_b2i8:
                  case nir_op_b2i16:
                  case nir_op_b2i32:
                  case nir_op_b2i64:
                  case nir_op_b2b32:
                  case nir_op_b2f16:
                  case nir_op_b2f32:
                  case nir_op_mov:
                     type = nir_dest_is_divergent(alu_instr->dest.dest) ? RegType::vgpr : RegType::sgpr;
                     break;
                  case nir_op_bcsel:
                     type = nir_dest_is_divergent(alu_instr->dest.dest) ? RegType::vgpr : RegType::sgpr;
                     /* fallthrough */
                  default:
                     for (unsigned i = 0; i < nir_op_infos[alu_instr->op].num_inputs; i++) {
                        if (allocated[alu_instr->src[i].src.ssa->index].type() == RegType::vgpr)
                           type = RegType::vgpr;
                     }
                     break;
               }

               RegClass rc = get_reg_class(ctx, type, alu_instr->dest.dest.ssa.num_components, alu_instr->dest.dest.ssa.bit_size);
               allocated[alu_instr->dest.dest.ssa.index] = Temp(0, rc);
               break;
            }
            case nir_instr_type_load_const: {
               unsigned num_components = nir_instr_as_load_const(instr)->def.num_components;
               unsigned bit_size = nir_instr_as_load_const(instr)->def.bit_size;
               RegClass rc = get_reg_class(ctx, RegType::sgpr, num_components, bit_size);
               allocated[nir_instr_as_load_const(instr)->def.index] = Temp(0, rc);
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);
               if (!nir_intrinsic_infos[intrinsic->intrinsic].has_dest)
                  break;
               RegType type = RegType::sgpr;
               switch(intrinsic->intrinsic) {
                  case nir_intrinsic_load_push_constant:
                  case nir_intrinsic_load_work_group_id:
                  case nir_intrinsic_load_num_work_groups:
                  case nir_intrinsic_load_subgroup_id:
                  case nir_intrinsic_load_num_subgroups:
                  case nir_intrinsic_load_first_vertex:
                  case nir_intrinsic_load_base_instance:
                  case nir_intrinsic_get_buffer_size:
                  case nir_intrinsic_vote_all:
                  case nir_intrinsic_vote_any:
                  case nir_intrinsic_read_first_invocation:
                  case nir_intrinsic_read_invocation:
                  case nir_intrinsic_first_invocation:
                  case nir_intrinsic_ballot:
                     type = RegType::sgpr;
                     break;
                  case nir_intrinsic_load_sample_id:
                  case nir_intrinsic_load_sample_mask_in:
                  case nir_intrinsic_load_input:
                  case nir_intrinsic_load_output:
                  case nir_intrinsic_load_input_vertex:
                  case nir_intrinsic_load_per_vertex_input:
                  case nir_intrinsic_load_per_vertex_output:
                  case nir_intrinsic_load_vertex_id:
                  case nir_intrinsic_load_vertex_id_zero_base:
                  case nir_intrinsic_load_barycentric_sample:
                  case nir_intrinsic_load_barycentric_pixel:
                  case nir_intrinsic_load_barycentric_model:
                  case nir_intrinsic_load_barycentric_centroid:
                  case nir_intrinsic_load_barycentric_at_sample:
                  case nir_intrinsic_load_barycentric_at_offset:
                  case nir_intrinsic_load_interpolated_input:
                  case nir_intrinsic_load_frag_coord:
                  case nir_intrinsic_load_sample_pos:
                  case nir_intrinsic_load_layer_id:
                  case nir_intrinsic_load_local_invocation_id:
                  case nir_intrinsic_load_local_invocation_index:
                  case nir_intrinsic_load_subgroup_invocation:
                  case nir_intrinsic_load_tess_coord:
                  case nir_intrinsic_write_invocation_amd:
                  case nir_intrinsic_mbcnt_amd:
                  case nir_intrinsic_load_instance_id:
                  case nir_intrinsic_ssbo_atomic_add:
                  case nir_intrinsic_ssbo_atomic_imin:
                  case nir_intrinsic_ssbo_atomic_umin:
                  case nir_intrinsic_ssbo_atomic_imax:
                  case nir_intrinsic_ssbo_atomic_umax:
                  case nir_intrinsic_ssbo_atomic_and:
                  case nir_intrinsic_ssbo_atomic_or:
                  case nir_intrinsic_ssbo_atomic_xor:
                  case nir_intrinsic_ssbo_atomic_exchange:
                  case nir_intrinsic_ssbo_atomic_comp_swap:
                  case nir_intrinsic_global_atomic_add:
                  case nir_intrinsic_global_atomic_imin:
                  case nir_intrinsic_global_atomic_umin:
                  case nir_intrinsic_global_atomic_imax:
                  case nir_intrinsic_global_atomic_umax:
                  case nir_intrinsic_global_atomic_and:
                  case nir_intrinsic_global_atomic_or:
                  case nir_intrinsic_global_atomic_xor:
                  case nir_intrinsic_global_atomic_exchange:
                  case nir_intrinsic_global_atomic_comp_swap:
                  case nir_intrinsic_image_deref_atomic_add:
                  case nir_intrinsic_image_deref_atomic_umin:
                  case nir_intrinsic_image_deref_atomic_imin:
                  case nir_intrinsic_image_deref_atomic_umax:
                  case nir_intrinsic_image_deref_atomic_imax:
                  case nir_intrinsic_image_deref_atomic_and:
                  case nir_intrinsic_image_deref_atomic_or:
                  case nir_intrinsic_image_deref_atomic_xor:
                  case nir_intrinsic_image_deref_atomic_exchange:
                  case nir_intrinsic_image_deref_atomic_comp_swap:
                  case nir_intrinsic_image_deref_size:
                  case nir_intrinsic_shared_atomic_add:
                  case nir_intrinsic_shared_atomic_imin:
                  case nir_intrinsic_shared_atomic_umin:
                  case nir_intrinsic_shared_atomic_imax:
                  case nir_intrinsic_shared_atomic_umax:
                  case nir_intrinsic_shared_atomic_and:
                  case nir_intrinsic_shared_atomic_or:
                  case nir_intrinsic_shared_atomic_xor:
                  case nir_intrinsic_shared_atomic_exchange:
                  case nir_intrinsic_shared_atomic_comp_swap:
                  case nir_intrinsic_shared_atomic_fadd:
                  case nir_intrinsic_load_scratch:
                  case nir_intrinsic_load_invocation_id:
                  case nir_intrinsic_load_primitive_id:
                     type = RegType::vgpr;
                     break;
                  case nir_intrinsic_shuffle:
                  case nir_intrinsic_quad_broadcast:
                  case nir_intrinsic_quad_swap_horizontal:
                  case nir_intrinsic_quad_swap_vertical:
                  case nir_intrinsic_quad_swap_diagonal:
                  case nir_intrinsic_quad_swizzle_amd:
                  case nir_intrinsic_masked_swizzle_amd:
                  case nir_intrinsic_inclusive_scan:
                  case nir_intrinsic_exclusive_scan:
                  case nir_intrinsic_reduce:
                  case nir_intrinsic_load_ubo:
                  case nir_intrinsic_load_ssbo:
                  case nir_intrinsic_load_global:
                  case nir_intrinsic_vulkan_resource_index:
                  case nir_intrinsic_load_shared:
                     type = nir_dest_is_divergent(intrinsic->dest) ? RegType::vgpr : RegType::sgpr;
                     break;
                  case nir_intrinsic_load_view_index:
                     type = ctx->stage == fragment_fs ? RegType::vgpr : RegType::sgpr;
                     break;
                  default:
                     for (unsigned i = 0; i < nir_intrinsic_infos[intrinsic->intrinsic].num_srcs; i++) {
                        if (allocated[intrinsic->src[i].ssa->index].type() == RegType::vgpr)
                           type = RegType::vgpr;
                     }
                     break;
               }
               RegClass rc = get_reg_class(ctx, type, intrinsic->dest.ssa.num_components, intrinsic->dest.ssa.bit_size);
               allocated[intrinsic->dest.ssa.index] = Temp(0, rc);

               switch(intrinsic->intrinsic) {
                  case nir_intrinsic_load_barycentric_sample:
                  case nir_intrinsic_load_barycentric_pixel:
                  case nir_intrinsic_load_barycentric_centroid:
                  case nir_intrinsic_load_barycentric_at_sample:
                  case nir_intrinsic_load_barycentric_at_offset: {
                     glsl_interp_mode mode = (glsl_interp_mode)nir_intrinsic_interp_mode(intrinsic);
                     spi_ps_inputs |= get_interp_input(intrinsic->intrinsic, mode);
                     break;
                  }
                  case nir_intrinsic_load_barycentric_model:
                     spi_ps_inputs |= S_0286CC_PERSP_PULL_MODEL_ENA(1);
                     break;
                  case nir_intrinsic_load_front_face:
                     spi_ps_inputs |= S_0286CC_FRONT_FACE_ENA(1);
                     break;
                  case nir_intrinsic_load_frag_coord:
                  case nir_intrinsic_load_sample_pos: {
                     uint8_t mask = nir_ssa_def_components_read(&intrinsic->dest.ssa);
                     for (unsigned i = 0; i < 4; i++) {
                        if (mask & (1 << i))
                           spi_ps_inputs |= S_0286CC_POS_X_FLOAT_ENA(1) << i;

                     }
                     break;
                  }
                  case nir_intrinsic_load_sample_id:
                     spi_ps_inputs |= S_0286CC_ANCILLARY_ENA(1);
                     break;
                  case nir_intrinsic_load_sample_mask_in:
                     spi_ps_inputs |= S_0286CC_ANCILLARY_ENA(1);
                     spi_ps_inputs |= S_0286CC_SAMPLE_COVERAGE_ENA(1);
                     break;
                  default:
                     break;
               }
               break;
            }
            case nir_instr_type_tex: {
               nir_tex_instr* tex = nir_instr_as_tex(instr);
               unsigned size = tex->dest.ssa.num_components;

               if (tex->dest.ssa.bit_size == 64)
                  size *= 2;
               if (tex->op == nir_texop_texture_samples) {
                  assert(!tex->dest.ssa.divergent);
               }
               if (nir_dest_is_divergent(tex->dest))
                  allocated[tex->dest.ssa.index] = Temp(0, RegClass(RegType::vgpr, size));
               else
                  allocated[tex->dest.ssa.index] = Temp(0, RegClass(RegType::sgpr, size));
               break;
            }
            case nir_instr_type_parallel_copy: {
               nir_foreach_parallel_copy_entry(entry, nir_instr_as_parallel_copy(instr)) {
                  allocated[entry->dest.ssa.index] = allocated[entry->src.ssa->index];
               }
               break;
            }
            case nir_instr_type_ssa_undef: {
               unsigned num_components = nir_instr_as_ssa_undef(instr)->def.num_components;
               unsigned bit_size = nir_instr_as_ssa_undef(instr)->def.bit_size;
               RegClass rc = get_reg_class(ctx, RegType::sgpr, num_components, bit_size);
               allocated[nir_instr_as_ssa_undef(instr)->def.index] = Temp(0, rc);
               break;
            }
            case nir_instr_type_phi: {
               nir_phi_instr* phi = nir_instr_as_phi(instr);
               RegType type;
               unsigned size = phi->dest.ssa.num_components;

               if (phi->dest.ssa.bit_size == 1) {
                  assert(size == 1 && "multiple components not yet supported on boolean phis.");
                  type = RegType::sgpr;
                  size *= lane_mask_size;
                  allocated[phi->dest.ssa.index] = Temp(0, RegClass(type, size));
                  break;
               }

               if (nir_dest_is_divergent(phi->dest)) {
                  type = RegType::vgpr;
               } else {
                  type = RegType::sgpr;
                  nir_foreach_phi_src (src, phi) {
                     if (allocated[src->src.ssa->index].type() == RegType::vgpr)
                        type = RegType::vgpr;
                     if (allocated[src->src.ssa->index].type() == RegType::none)
                        done = false;
                  }
               }

               RegClass rc = get_reg_class(ctx, type, phi->dest.ssa.num_components, phi->dest.ssa.bit_size);
               if (rc != allocated[phi->dest.ssa.index].regClass()) {
                  done = false;
               } else {
                  nir_foreach_phi_src(src, phi)
                     assert(allocated[src->src.ssa->index].size() == rc.size());
               }
               allocated[phi->dest.ssa.index] = Temp(0, rc);
               break;
            }
            default:
               break;
            }
         }
      }
   }

   if (G_0286CC_POS_W_FLOAT_ENA(spi_ps_inputs)) {
      /* If POS_W_FLOAT (11) is enabled, at least one of PERSP_* must be enabled too */
      spi_ps_inputs |= S_0286CC_PERSP_CENTER_ENA(1);
   }

   if (!(spi_ps_inputs & 0x7F)) {
      /* At least one of PERSP_* (0xF) or LINEAR_* (0x70) must be enabled */
      spi_ps_inputs |= S_0286CC_PERSP_CENTER_ENA(1);
   }

   ctx->program->config->spi_ps_input_ena = spi_ps_inputs;
   ctx->program->config->spi_ps_input_addr = spi_ps_inputs;

   for (unsigned i = 0; i < impl->ssa_alloc; i++)
      allocated[i] = Temp(ctx->program->allocateId(), allocated[i].regClass());

   ctx->allocated.reset(allocated.release());
   ctx->cf_info.nir_to_aco.reset(nir_to_aco.release());

   /* align and copy constant data */
   while (ctx->program->constant_data.size() % 4u)
      ctx->program->constant_data.push_back(0);
   ctx->constant_data_offset = ctx->program->constant_data.size();
   ctx->program->constant_data.insert(ctx->program->constant_data.end(),
                                      (uint8_t*)shader->constant_data,
                                      (uint8_t*)shader->constant_data + shader->constant_data_size);
}

Pseudo_instruction *add_startpgm(struct isel_context *ctx)
{
   unsigned arg_count = ctx->args->ac.arg_count;
   if (ctx->stage == fragment_fs) {
      /* LLVM optimizes away unused FS inputs and computes spi_ps_input_addr
       * itself and then communicates the results back via the ELF binary.
       * Mirror what LLVM does by re-mapping the VGPR arguments here.
       *
       * TODO: If we made the FS input scanning code into a separate pass that
       * could run before argument setup, then this wouldn't be necessary
       * anymore.
       */
      struct ac_shader_args *args = &ctx->args->ac;
      arg_count = 0;
      for (unsigned i = 0, vgpr_arg = 0, vgpr_reg = 0; i < args->arg_count; i++) {
         if (args->args[i].file != AC_ARG_VGPR) {
            arg_count++;
            continue;
         }

         if (!(ctx->program->config->spi_ps_input_addr & (1 << vgpr_arg))) {
            args->args[i].skip = true;
         } else {
            args->args[i].offset = vgpr_reg;
            vgpr_reg += args->args[i].size;
            arg_count++;
         }
         vgpr_arg++;
      }
   }

   aco_ptr<Pseudo_instruction> startpgm{create_instruction<Pseudo_instruction>(aco_opcode::p_startpgm, Format::PSEUDO, 0, arg_count + 1)};
   for (unsigned i = 0, arg = 0; i < ctx->args->ac.arg_count; i++) {
      if (ctx->args->ac.args[i].skip)
         continue;

      enum ac_arg_regfile file = ctx->args->ac.args[i].file;
      unsigned size = ctx->args->ac.args[i].size;
      unsigned reg = ctx->args->ac.args[i].offset;
      RegClass type = RegClass(file == AC_ARG_SGPR ? RegType::sgpr : RegType::vgpr, size);
      Temp dst = Temp{ctx->program->allocateId(), type};
      ctx->arg_temps[i] = dst;
      startpgm->definitions[arg] = Definition(dst);
      startpgm->definitions[arg].setFixed(PhysReg{file == AC_ARG_SGPR ? reg : reg + 256});
      arg++;
   }
   startpgm->definitions[arg_count] = Definition{ctx->program->allocateId(), exec, ctx->program->lane_mask};
   Pseudo_instruction *instr = startpgm.get();
   ctx->block->instructions.push_back(std::move(startpgm));

   /* Stash these in the program so that they can be accessed later when
    * handling spilling.
    */
   ctx->program->private_segment_buffer = get_arg(ctx, ctx->args->ring_offsets);
   ctx->program->scratch_offset = get_arg(ctx, ctx->args->scratch_offset);

   return instr;
}

int
type_size(const struct glsl_type *type, bool bindless)
{
   // TODO: don't we need type->std430_base_alignment() here?
   return glsl_count_attribute_slots(type, false);
}

void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type)
      ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length,
   *align = comp_size;
}

static bool
mem_vectorize_callback(unsigned align, unsigned bit_size,
                       unsigned num_components, unsigned high_offset,
                       nir_intrinsic_instr *low, nir_intrinsic_instr *high)
{
   if (num_components > 4)
      return false;

   /* >128 bit loads are split except with SMEM */
   if (bit_size * num_components > 128)
      return false;

   switch (low->intrinsic) {
   case nir_intrinsic_load_global:
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_push_constant:
      return align % (bit_size == 8 ? 2 : 4) == 0;
   case nir_intrinsic_load_deref:
   case nir_intrinsic_store_deref:
      assert(nir_src_as_deref(low->src[0])->mode == nir_var_mem_shared);
      /* fallthrough */
   case nir_intrinsic_load_shared:
   case nir_intrinsic_store_shared:
      if (bit_size * num_components > 64) /* 96 and 128 bit loads require 128 bit alignment and are split otherwise */
         return align % 16 == 0;
      else
         return align % (bit_size == 8 ? 2 : 4) == 0;
   default:
      return false;
   }
   return false;
}

void
setup_vs_output_info(isel_context *ctx, nir_shader *nir,
                     bool export_prim_id, bool export_clip_dists,
                     radv_vs_output_info *outinfo)
{
   memset(outinfo->vs_output_param_offset, AC_EXP_PARAM_UNDEFINED,
          sizeof(outinfo->vs_output_param_offset));

   outinfo->param_exports = 0;
   int pos_written = 0x1;
   if (outinfo->writes_pointsize || outinfo->writes_viewport_index || outinfo->writes_layer)
      pos_written |= 1 << 1;

   uint64_t mask = nir->info.outputs_written;
   while (mask) {
      int idx = u_bit_scan64(&mask);
      if (idx >= VARYING_SLOT_VAR0 || idx == VARYING_SLOT_LAYER ||
          idx == VARYING_SLOT_PRIMITIVE_ID || idx == VARYING_SLOT_VIEWPORT ||
          ((idx == VARYING_SLOT_CLIP_DIST0 || idx == VARYING_SLOT_CLIP_DIST1) && export_clip_dists)) {
         if (outinfo->vs_output_param_offset[idx] == AC_EXP_PARAM_UNDEFINED)
            outinfo->vs_output_param_offset[idx] = outinfo->param_exports++;
      }
   }
   if (outinfo->writes_layer &&
       outinfo->vs_output_param_offset[VARYING_SLOT_LAYER] == AC_EXP_PARAM_UNDEFINED) {
      /* when ctx->options->key.has_multiview_view_index = true, the layer
       * variable isn't declared in NIR and it's isel's job to get the layer */
      outinfo->vs_output_param_offset[VARYING_SLOT_LAYER] = outinfo->param_exports++;
   }

   if (export_prim_id) {
      assert(outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] == AC_EXP_PARAM_UNDEFINED);
      outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] = outinfo->param_exports++;
   }

   ctx->export_clip_dists = export_clip_dists;
   ctx->num_clip_distances = util_bitcount(outinfo->clip_dist_mask);
   ctx->num_cull_distances = util_bitcount(outinfo->cull_dist_mask);

   assert(ctx->num_clip_distances + ctx->num_cull_distances <= 8);

   if (ctx->num_clip_distances + ctx->num_cull_distances > 0)
      pos_written |= 1 << 2;
   if (ctx->num_clip_distances + ctx->num_cull_distances > 4)
      pos_written |= 1 << 3;

   outinfo->pos_exports = util_bitcount(pos_written);
}

void
setup_vs_variables(isel_context *ctx, nir_shader *nir)
{
   nir_foreach_shader_in_variable(variable, nir)
   {
      variable->data.driver_location = variable->data.location * 4;
   }
   nir_foreach_shader_out_variable(variable, nir)
   {
      if (ctx->stage == vertex_vs || ctx->stage == ngg_vertex_gs)
         variable->data.driver_location = variable->data.location * 4;

      assert(variable->data.location >= 0 && variable->data.location <= UINT8_MAX);
      ctx->output_drv_loc_to_var_slot[MESA_SHADER_VERTEX][variable->data.driver_location / 4] = variable->data.location;
   }

   if (ctx->stage == vertex_vs || ctx->stage == ngg_vertex_gs) {
      radv_vs_output_info *outinfo = &ctx->program->info->vs.outinfo;
      setup_vs_output_info(ctx, nir, outinfo->export_prim_id,
                           ctx->options->key.vs_common_out.export_clip_dists, outinfo);
   } else if (ctx->stage == vertex_ls) {
      ctx->tcs_num_inputs = ctx->program->info->vs.num_linked_outputs;
   }

   if (ctx->stage == ngg_vertex_gs && ctx->args->options->key.vs_common_out.export_prim_id) {
      /* We need to store the primitive IDs in LDS */
      unsigned lds_size = ctx->program->info->ngg_info.esgs_ring_size;
      ctx->program->config->lds_size = (lds_size + ctx->program->lds_alloc_granule - 1) /
                                       ctx->program->lds_alloc_granule;
   }
}

void setup_gs_variables(isel_context *ctx, nir_shader *nir)
{
   if (ctx->stage == vertex_geometry_gs || ctx->stage == tess_eval_geometry_gs)
      ctx->program->config->lds_size = ctx->program->info->gs_ring_info.lds_size; /* Already in units of the alloc granularity */

   nir_foreach_shader_out_variable(variable, nir) {
      variable->data.driver_location = variable->data.location * 4;
   }

   if (ctx->stage == vertex_geometry_gs)
      ctx->program->info->gs.es_type = MESA_SHADER_VERTEX;
   else if (ctx->stage == tess_eval_geometry_gs)
      ctx->program->info->gs.es_type = MESA_SHADER_TESS_EVAL;
}

void
setup_tcs_info(isel_context *ctx, nir_shader *nir, nir_shader *vs)
{
   /* When the number of TCS input and output vertices are the same (typically 3):
    * - There is an equal amount of LS and HS invocations
    * - In case of merged LSHS shaders, the LS and HS halves of the shader
    *   always process the exact same vertex. We can use this knowledge to optimize them.
    *
    * We don't set tcs_in_out_eq if the float controls differ because that might
    * involve different float modes for the same block and our optimizer
    * doesn't handle a instruction dominating another with a different mode.
    */
   ctx->tcs_in_out_eq =
      ctx->stage == vertex_tess_control_hs &&
      ctx->args->options->key.tcs.input_vertices == nir->info.tess.tcs_vertices_out &&
      vs->info.float_controls_execution_mode == nir->info.float_controls_execution_mode;

   if (ctx->tcs_in_out_eq) {
      ctx->tcs_temp_only_inputs = ~nir->info.tess.tcs_cross_invocation_inputs_read &
                                    ~nir->info.inputs_read_indirectly &
                                    nir->info.inputs_read;
   }

   ctx->tcs_num_inputs = ctx->program->info->tcs.num_linked_inputs;
   ctx->tcs_num_outputs = ctx->program->info->tcs.num_linked_outputs;
   ctx->tcs_num_patch_outputs = ctx->program->info->tcs.num_linked_patch_outputs;

   ctx->tcs_num_patches = get_tcs_num_patches(
                             ctx->args->options->key.tcs.input_vertices,
                             nir->info.tess.tcs_vertices_out,
                             ctx->tcs_num_inputs,
                             ctx->tcs_num_outputs,
                             ctx->tcs_num_patch_outputs,
                             ctx->args->options->tess_offchip_block_dw_size,
                             ctx->args->options->chip_class,
                             ctx->args->options->family);
   unsigned lds_size = calculate_tess_lds_size(
                             ctx->args->options->chip_class,
                             ctx->args->options->key.tcs.input_vertices,
                             nir->info.tess.tcs_vertices_out,
                             ctx->tcs_num_inputs,
                             ctx->tcs_num_patches,
                             ctx->tcs_num_outputs,
                             ctx->tcs_num_patch_outputs);

   ctx->args->shader_info->tcs.num_patches = ctx->tcs_num_patches;
   ctx->args->shader_info->tcs.num_lds_blocks = lds_size;
   ctx->program->config->lds_size = (lds_size + ctx->program->lds_alloc_granule - 1) /
                                    ctx->program->lds_alloc_granule;
}

void
setup_tcs_variables(isel_context *ctx, nir_shader *nir)
{
   nir_foreach_shader_out_variable(variable, nir) {
      assert(variable->data.location >= 0 && variable->data.location <= UINT8_MAX);

      if (variable->data.location == VARYING_SLOT_TESS_LEVEL_OUTER)
         ctx->tcs_tess_lvl_out_loc = variable->data.driver_location * 4u;
      else if (variable->data.location == VARYING_SLOT_TESS_LEVEL_INNER)
         ctx->tcs_tess_lvl_in_loc = variable->data.driver_location * 4u;

      if (variable->data.patch)
         ctx->output_tcs_patch_drv_loc_to_var_slot[variable->data.driver_location / 4] = variable->data.location;
      else
         ctx->output_drv_loc_to_var_slot[MESA_SHADER_TESS_CTRL][variable->data.driver_location / 4] = variable->data.location;
   }
}

void
setup_tes_variables(isel_context *ctx, nir_shader *nir)
{
   ctx->tcs_num_patches = ctx->args->options->key.tes.num_patches;
   ctx->tcs_num_outputs = ctx->program->info->tes.num_linked_inputs;

   nir_foreach_shader_out_variable(variable, nir) {
      if (ctx->stage == tess_eval_vs || ctx->stage == ngg_tess_eval_gs)
         variable->data.driver_location = variable->data.location * 4;
   }

   if (ctx->stage == tess_eval_vs || ctx->stage == ngg_tess_eval_gs) {
      radv_vs_output_info *outinfo = &ctx->program->info->tes.outinfo;
      setup_vs_output_info(ctx, nir, outinfo->export_prim_id,
                           ctx->options->key.vs_common_out.export_clip_dists, outinfo);
   }
}

void
setup_variables(isel_context *ctx, nir_shader *nir)
{
   switch (nir->info.stage) {
   case MESA_SHADER_FRAGMENT: {
      nir_foreach_shader_out_variable(variable, nir)
      {
         int idx = variable->data.location + variable->data.index;
         variable->data.driver_location = idx * 4;
      }
      break;
   }
   case MESA_SHADER_COMPUTE: {
      ctx->program->config->lds_size = (nir->info.cs.shared_size + ctx->program->lds_alloc_granule - 1) /
                                       ctx->program->lds_alloc_granule;
      break;
   }
   case MESA_SHADER_VERTEX: {
      setup_vs_variables(ctx, nir);
      break;
   }
   case MESA_SHADER_GEOMETRY: {
      setup_gs_variables(ctx, nir);
      break;
   }
   case MESA_SHADER_TESS_CTRL: {
      setup_tcs_variables(ctx, nir);
      break;
   }
   case MESA_SHADER_TESS_EVAL: {
      setup_tes_variables(ctx, nir);
      break;
   }
   default:
      unreachable("Unhandled shader stage.");
   }
}

unsigned
lower_bit_size_callback(const nir_alu_instr *alu, void *_)
{
   if (nir_op_is_vec(alu->op))
      return 0;

   unsigned bit_size = alu->dest.dest.ssa.bit_size;
   if (nir_alu_instr_is_comparison(alu))
      bit_size = nir_src_bit_size(alu->src[0].src);

   if (bit_size >= 32 || bit_size == 1)
      return 0;

   if (alu->op == nir_op_bcsel)
      return 0;

   const nir_op_info *info = &nir_op_infos[alu->op];

   if (info->is_conversion)
      return 0;

   bool is_integer = info->output_type & (nir_type_uint | nir_type_int);
   for (unsigned i = 0; is_integer && (i < info->num_inputs); i++)
      is_integer = info->input_types[i] & (nir_type_uint | nir_type_int);

   return is_integer ? 32 : 0;
}

void
setup_nir(isel_context *ctx, nir_shader *nir)
{
   /* the variable setup has to be done before lower_io / CSE */
   setup_variables(ctx, nir);

   /* optimize and lower memory operations */
   if (nir_lower_explicit_io(nir, nir_var_mem_global, nir_address_format_64bit_global)) {
      nir_opt_constant_folding(nir);
      nir_opt_cse(nir);
   }

   bool lower_to_scalar = false;
   bool lower_pack = false;
   nir_variable_mode robust_modes = (nir_variable_mode)0;

   if (ctx->options->robust_buffer_access) {
      robust_modes = (nir_variable_mode)(nir_var_mem_ubo |
                                         nir_var_mem_ssbo |
                                         nir_var_mem_global |
                                         nir_var_mem_push_const);
   }

   if (nir_opt_load_store_vectorize(nir,
                                    (nir_variable_mode)(nir_var_mem_ssbo | nir_var_mem_ubo |
                                                        nir_var_mem_push_const | nir_var_mem_shared |
                                                        nir_var_mem_global),
                                    mem_vectorize_callback, robust_modes)) {
      lower_to_scalar = true;
      lower_pack = true;
   }
   if (nir->info.stage != MESA_SHADER_COMPUTE)
      nir_lower_io(nir, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), type_size, (nir_lower_io_options)0);

   lower_to_scalar |= nir_opt_shrink_vectors(nir);

   if (lower_to_scalar)
      nir_lower_alu_to_scalar(nir, NULL, NULL);
   if (lower_pack)
      nir_lower_pack(nir);

   /* lower ALU operations */
   nir_lower_int64(nir);

   if (nir_lower_bit_size(nir, lower_bit_size_callback, NULL))
      nir_copy_prop(nir); /* allow nir_opt_idiv_const() to optimize lowered divisions */

   nir_opt_idiv_const(nir, 32);
   nir_lower_idiv(nir, nir_lower_idiv_precise);

   /* optimize the lowered ALU operations */
   bool more_algebraic = true;
   while (more_algebraic) {
      more_algebraic = false;
      NIR_PASS_V(nir, nir_copy_prop);
      NIR_PASS_V(nir, nir_opt_dce);
      NIR_PASS_V(nir, nir_opt_constant_folding);
      NIR_PASS(more_algebraic, nir, nir_opt_algebraic);
   }

   /* Do late algebraic optimization to turn add(a, neg(b)) back into
    * subs, then the mandatory cleanup after algebraic.  Note that it may
    * produce fnegs, and if so then we need to keep running to squash
    * fneg(fneg(a)).
    */
   bool more_late_algebraic = true;
   while (more_late_algebraic) {
      more_late_algebraic = false;
      NIR_PASS(more_late_algebraic, nir, nir_opt_algebraic_late);
      NIR_PASS_V(nir, nir_opt_constant_folding);
      NIR_PASS_V(nir, nir_copy_prop);
      NIR_PASS_V(nir, nir_opt_dce);
      NIR_PASS_V(nir, nir_opt_cse);
   }

   /* cleanup passes */
   nir_lower_load_const_to_scalar(nir);
   nir_move_options move_opts = (nir_move_options)(
      nir_move_const_undef | nir_move_load_ubo | nir_move_load_input |
      nir_move_comparisons | nir_move_copies);
   nir_opt_sink(nir, move_opts);
   nir_opt_move(nir, move_opts);
   nir_convert_to_lcssa(nir, true, false);
   nir_lower_phis_to_scalar(nir);

   nir_function_impl *func = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(func);
}

void
setup_xnack(Program *program)
{
   switch (program->family) {
   /* GFX8 APUs */
   case CHIP_CARRIZO:
   case CHIP_STONEY:
   /* GFX9 APUS */
   case CHIP_RAVEN:
   case CHIP_RAVEN2:
   case CHIP_RENOIR:
      program->xnack_enabled = true;
      break;
   default:
      break;
   }
}

isel_context
setup_isel_context(Program* program,
                   unsigned shader_count,
                   struct nir_shader *const *shaders,
                   ac_shader_config* config,
                   struct radv_shader_args *args,
                   bool is_gs_copy_shader)
{
   Stage stage = 0;
   for (unsigned i = 0; i < shader_count; i++) {
      switch (shaders[i]->info.stage) {
      case MESA_SHADER_VERTEX:
         stage |= sw_vs;
         break;
      case MESA_SHADER_TESS_CTRL:
         stage |= sw_tcs;
         break;
      case MESA_SHADER_TESS_EVAL:
         stage |= sw_tes;
         break;
      case MESA_SHADER_GEOMETRY:
         stage |= is_gs_copy_shader ? sw_gs_copy : sw_gs;
         break;
      case MESA_SHADER_FRAGMENT:
         stage |= sw_fs;
         break;
      case MESA_SHADER_COMPUTE:
         stage |= sw_cs;
         break;
      default:
         unreachable("Shader stage not implemented");
      }
   }
   bool gfx9_plus = args->options->chip_class >= GFX9;
   bool ngg = args->shader_info->is_ngg && args->options->chip_class >= GFX10;
   if (stage == sw_vs && args->shader_info->vs.as_es && !ngg)
      stage |= hw_es;
   else if (stage == sw_vs && !args->shader_info->vs.as_ls && !ngg)
      stage |= hw_vs;
   else if (stage == sw_vs && ngg)
      stage |= hw_ngg_gs; /* GFX10/NGG: VS without GS uses the HW GS stage */
   else if (stage == sw_gs)
      stage |= hw_gs;
   else if (stage == sw_fs)
      stage |= hw_fs;
   else if (stage == sw_cs)
      stage |= hw_cs;
   else if (stage == sw_gs_copy)
      stage |= hw_vs;
   else if (stage == (sw_vs | sw_gs) && gfx9_plus && !ngg)
      stage |= hw_gs;
   else if (stage == sw_vs && args->shader_info->vs.as_ls)
      stage |= hw_ls; /* GFX6-8: VS is a Local Shader, when tessellation is used */
   else if (stage == sw_tcs)
      stage |= hw_hs; /* GFX6-8: TCS is a Hull Shader */
   else if (stage == (sw_vs | sw_tcs))
      stage |= hw_hs; /* GFX9-10: VS+TCS merged into a Hull Shader */
   else if (stage == sw_tes && !args->shader_info->tes.as_es && !ngg)
      stage |= hw_vs; /* GFX6-9: TES without GS uses the HW VS stage (and GFX10/legacy) */
   else if (stage == sw_tes && !args->shader_info->tes.as_es && ngg)
      stage |= hw_ngg_gs; /* GFX10/NGG: TES without GS uses the HW GS stage */
   else if (stage == sw_tes && args->shader_info->tes.as_es && !ngg)
      stage |= hw_es; /* GFX6-8: TES is an Export Shader */
   else if (stage == (sw_tes | sw_gs) && gfx9_plus && !ngg)
      stage |= hw_gs; /* GFX9: TES+GS merged into a GS (and GFX10/legacy) */
   else
      unreachable("Shader stage not implemented");

   init_program(program, stage, args->shader_info,
                args->options->chip_class, args->options->family, config);

   isel_context ctx = {};
   ctx.program = program;
   ctx.args = args;
   ctx.options = args->options;
   ctx.stage = program->stage;

   /* TODO: Check if we need to adjust min_waves for unknown workgroup sizes. */
   if (program->stage & (hw_vs | hw_fs)) {
      /* PS and legacy VS have separate waves, no workgroups */
      program->workgroup_size = program->wave_size;
   } else if (program->stage == compute_cs) {
      /* CS sets the workgroup size explicitly */
      unsigned* bsize = program->info->cs.block_size;
      program->workgroup_size = bsize[0] * bsize[1] * bsize[2];
   } else if ((program->stage & hw_es) || program->stage == geometry_gs) {
      /* Unmerged ESGS operate in workgroups if on-chip GS (LDS rings) are enabled on GFX7-8 (not implemented in Mesa)  */
      program->workgroup_size = program->wave_size;
   } else if (program->stage & hw_gs) {
      /* If on-chip GS (LDS rings) are enabled on GFX9 or later, merged GS operates in workgroups */
      assert(program->chip_class >= GFX9);
      uint32_t es_verts_per_subgrp = G_028A44_ES_VERTS_PER_SUBGRP(program->info->gs_ring_info.vgt_gs_onchip_cntl);
      uint32_t gs_instr_prims_in_subgrp = G_028A44_GS_INST_PRIMS_IN_SUBGRP(program->info->gs_ring_info.vgt_gs_onchip_cntl);
      uint32_t workgroup_size = MAX2(es_verts_per_subgrp, gs_instr_prims_in_subgrp);
      program->workgroup_size = MAX2(MIN2(workgroup_size, 256), 1);
   } else if (program->stage == vertex_ls) {
      /* Unmerged LS operates in workgroups */
      program->workgroup_size = UINT_MAX; /* TODO: probably tcs_num_patches * tcs_vertices_in, but those are not plumbed to ACO for LS */
   } else if (program->stage == tess_control_hs) {
      /* Unmerged HS operates in workgroups, size is determined by the output vertices */
      setup_tcs_info(&ctx, shaders[0], NULL);
      program->workgroup_size = ctx.tcs_num_patches * shaders[0]->info.tess.tcs_vertices_out;
   } else if (program->stage == vertex_tess_control_hs) {
      /* Merged LSHS operates in workgroups, but can still have a different number of LS and HS invocations */
      setup_tcs_info(&ctx, shaders[1], shaders[0]);
      program->workgroup_size = ctx.tcs_num_patches * MAX2(shaders[1]->info.tess.tcs_vertices_out, ctx.args->options->key.tcs.input_vertices);
   } else if (program->stage & hw_ngg_gs) {
      /* TODO: Calculate workgroup size of NGG shaders. */
      program->workgroup_size = UINT_MAX;
   } else {
      unreachable("Unsupported shader stage.");
   }

   calc_min_waves(program);
   program->vgpr_limit = get_addr_vgpr_from_waves(program, program->min_waves);
   program->sgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);

   unsigned scratch_size = 0;
   if (program->stage == gs_copy_vs) {
      assert(shader_count == 1);
      setup_vs_output_info(&ctx, shaders[0], false, true, &args->shader_info->vs.outinfo);
   } else {
      for (unsigned i = 0; i < shader_count; i++) {
         nir_shader *nir = shaders[i];
         setup_nir(&ctx, nir);
      }

      for (unsigned i = 0; i < shader_count; i++)
         scratch_size = std::max(scratch_size, shaders[i]->scratch_size);
   }

   ctx.program->config->scratch_bytes_per_wave = align(scratch_size * ctx.program->wave_size, 1024);

   ctx.block = ctx.program->create_and_insert_block();
   ctx.block->loop_nest_depth = 0;
   ctx.block->kind = block_kind_top_level;

   setup_xnack(program);
   program->sram_ecc_enabled = args->options->family == CHIP_ARCTURUS;
   /* apparently gfx702 also has fast v_fma_f32 but I can't find a family for that */
   program->has_fast_fma32 = program->chip_class >= GFX9;
   if (args->options->family == CHIP_TAHITI || args->options->family == CHIP_CARRIZO || args->options->family == CHIP_HAWAII)
      program->has_fast_fma32 = true;

   return ctx;
}

}

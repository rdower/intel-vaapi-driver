/*
 * Copyright � 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Li Xiaowei <xiaowei.a.li@intel.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "intel_batchbuffer.h"
#include "intel_driver.h"

#include "i965_defines.h"
#include "i965_drv_video.h"
#include "gen75_vpp_gpe.h"

#define MAX_INTERFACE_DESC_GEN6      MAX_GPE_KERNELS
#define MAX_MEDIA_SURFACES_GEN6      34

#define SURFACE_STATE_PADDED_SIZE_0_GEN7        ALIGN(sizeof(struct gen7_surface_state), 32)
#define SURFACE_STATE_PADDED_SIZE_1_GEN7        ALIGN(sizeof(struct gen7_surface_state2), 32)
#define SURFACE_STATE_PADDED_SIZE               MAX(SURFACE_STATE_PADDED_SIZE_0_GEN7, SURFACE_STATE_PADDED_SIZE_1_GEN7)

#define SURFACE_STATE_OFFSET(index)             (SURFACE_STATE_PADDED_SIZE * (index))
#define BINDING_TABLE_OFFSET(index)             (SURFACE_STATE_OFFSET(MAX_MEDIA_SURFACES_GEN6) + sizeof(unsigned int) * (index))

#define CURBE_ALLOCATION_SIZE   37              
#define CURBE_TOTAL_DATA_LENGTH (4 * 32)        
#define CURBE_URB_ENTRY_LENGTH  4               

extern VAStatus 
i965_CreateSurfaces(VADriverContextP ctx,
                    int width,
                    int height,
                    int format,
                    int num_surfaces,
                    VASurfaceID *surfaces);

extern VAStatus 
i965_DestroySurfaces(VADriverContextP ctx,
                     VASurfaceID *surface_list,
                     int num_surfaces);

/* Shaders information for sharpening */
static const unsigned int gen75_gpe_sharpening_h_blur[][4] = {
   #include "shaders/post_processing/gen75/sharpening_h_blur.g75b"
};
static const unsigned int gen75_gpe_sharpening_v_blur[][4] = {
   #include "shaders/post_processing/gen75/sharpening_v_blur.g75b"
};
static const unsigned int gen75_gpe_sharpening_unmask[][4] = {
   #include "shaders/post_processing/gen75/sharpening_unmask.g75b"
};
static struct i965_kernel gen75_vpp_sharpening_kernels[] = {
    {
        "vpp: sharpening(horizontal blur)",
        VPP_GPE_SHARPENING,
        gen75_gpe_sharpening_h_blur, 			
        sizeof(gen75_gpe_sharpening_h_blur),		
        NULL
    },
    {
        "vpp: sharpening(vertical blur)",
        VPP_GPE_SHARPENING,
        gen75_gpe_sharpening_v_blur, 			
        sizeof(gen75_gpe_sharpening_v_blur),		
        NULL
    },
    {
        "vpp: sharpening(unmask)",
        VPP_GPE_SHARPENING,
        gen75_gpe_sharpening_unmask, 			
        sizeof(gen75_gpe_sharpening_unmask),		
        NULL
    },
}; 

static VAStatus
gpe_surfaces_setup(VADriverContextP ctx, 
                   struct vpp_gpe_context *vpp_gpe_ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface;
    unsigned int i = 0;
    unsigned char input_surface_sum = (1 + vpp_gpe_ctx->forward_surf_sum +
                                         vpp_gpe_ctx->backward_surf_sum) * 2;

    /* Binding input NV12 surfaces (Luma + Chroma)*/
    for( i = 0; i < input_surface_sum; i += 2){ 
         obj_surface = SURFACE(vpp_gpe_ctx->surface_input[i/2]);
         assert(obj_surface);
         vpp_gpe_ctx->vpp_media_rw_surface_setup(ctx,
                                                 &vpp_gpe_ctx->gpe_ctx,
                                                 obj_surface,
                                                 BINDING_TABLE_OFFSET(i),
                                                 SURFACE_STATE_OFFSET(i));

         vpp_gpe_ctx->vpp_media_chroma_surface_setup(ctx,
                                                     &vpp_gpe_ctx->gpe_ctx,
                                                     obj_surface,
                                                     BINDING_TABLE_OFFSET(i + 1),
                                                     SURFACE_STATE_OFFSET(i + 1));
    }

    /* Binding output NV12 surface(Luma + Chroma) */
    obj_surface = SURFACE(vpp_gpe_ctx->surface_output);
    assert(obj_surface);
    vpp_gpe_ctx->vpp_media_rw_surface_setup(ctx,
                                            &vpp_gpe_ctx->gpe_ctx,
                                            obj_surface,
                                            BINDING_TABLE_OFFSET(input_surface_sum),
                                            SURFACE_STATE_OFFSET(input_surface_sum));
    vpp_gpe_ctx->vpp_media_chroma_surface_setup(ctx,
                                                &vpp_gpe_ctx->gpe_ctx,
                                                obj_surface,
                                                BINDING_TABLE_OFFSET(input_surface_sum + 1),
                                                SURFACE_STATE_OFFSET(input_surface_sum + 1));
    /* Bind kernel return buffer surface */
    vpp_gpe_ctx->vpp_buffer_surface_setup(ctx,
                                          &vpp_gpe_ctx->gpe_ctx,
                                          &vpp_gpe_ctx->vpp_kernel_return,
                                          BINDING_TABLE_OFFSET((input_surface_sum + 2)),
                                          SURFACE_STATE_OFFSET(input_surface_sum + 2));

    return VA_STATUS_SUCCESS;
}

static VAStatus
gpe_interface_setup(VADriverContextP ctx, 
                    struct vpp_gpe_context *vpp_gpe_ctx)
{
    struct gen6_interface_descriptor_data *desc;   
    dri_bo *bo = vpp_gpe_ctx->gpe_ctx.idrt.bo;
    int i; 

    dri_bo_map(bo, 1);
    assert(bo->virtual);
    desc = bo->virtual;
    
    /*Setup the descritor table*/
    for(i = 0; i < vpp_gpe_ctx->sub_shader_sum; i++){
        struct i965_kernel *kernel = &vpp_gpe_ctx->gpe_ctx.kernels[i];
        assert(sizeof(*desc) == 32);
        memset(desc, 0, sizeof(*desc));
        desc->desc0.kernel_start_pointer = (kernel->bo->offset >> 6);
        desc->desc2.sampler_count = 0; /* FIXME: */
        desc->desc2.sampler_state_pointer = NULL;
        desc->desc3.binding_table_entry_count = 6; /* FIXME: */
        desc->desc3.binding_table_pointer = (BINDING_TABLE_OFFSET(0) >> 5);
        desc->desc4.constant_urb_entry_read_offset = 0;
        desc->desc4.constant_urb_entry_read_length = 0;

        dri_bo_emit_reloc(bo,	
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          0,
                          i* sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc0),
                          kernel->bo);
        desc++;
    }

    dri_bo_unmap(bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus 
gpe_constant_setup(VADriverContextP ctx, 
                   struct vpp_gpe_context *vpp_gpe_ctx){
    dri_bo_map(vpp_gpe_ctx->gpe_ctx.curbe.bo, 1);
    assert(vpp_gpe_ctx->gpe_ctx.curbe.bo->virtual);
    /*Copy buffer into CURB*/
    /*     
    unsigned char* constant_buffer = vpp_gpe_ctx->gpe_ctx.curbe.bo->virtual;	
    memcpy(constant_buffer, vpp_gpe_ctx->kernel_param, 
                            vpp_gpe_ctx->kernel_param_size);
    */  
    dri_bo_unmap(vpp_gpe_ctx->gpe_ctx.curbe.bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus 
gpe_fill_thread_parameters(VADriverContextP ctx,
                           struct vpp_gpe_context *vpp_gpe_ctx)
{
    unsigned int *command_ptr;
    unsigned int i, size = vpp_gpe_ctx->thread_param_size;
    unsigned char* position = NULL;

    /* Thread inline data setting*/
    dri_bo_map(vpp_gpe_ctx->vpp_batchbuffer.bo, 1);
    command_ptr = vpp_gpe_ctx->vpp_batchbuffer.bo->virtual;

    for(i = 0; i < vpp_gpe_ctx->thread_num; i ++)
    {
         *command_ptr++ = (CMD_MEDIA_OBJECT | (size/sizeof(int) + 6 - 2));
         *command_ptr++ = vpp_gpe_ctx->sub_shader_index;
         *command_ptr++ = 0;
         *command_ptr++ = 0;
         *command_ptr++ = 0;
         *command_ptr++ = 0;
   
         /* copy thread inline data */
         position =(unsigned char*)(vpp_gpe_ctx->thread_param + size * i);
         memcpy(command_ptr, position, size);
         command_ptr += size/sizeof(int);
    }   

    *command_ptr++ = 0;
    *command_ptr++ = MI_BATCH_BUFFER_END;

    dri_bo_unmap(vpp_gpe_ctx->vpp_batchbuffer.bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus
gpe_pipeline_setup(VADriverContextP ctx,
                   struct vpp_gpe_context *vpp_gpe_ctx)
{
    intel_batchbuffer_start_atomic(vpp_gpe_ctx->batch, 0x1000);
    intel_batchbuffer_emit_mi_flush(vpp_gpe_ctx->batch);

    gen6_gpe_pipeline_setup(ctx, &vpp_gpe_ctx->gpe_ctx, vpp_gpe_ctx->batch);
 
    gpe_fill_thread_parameters(ctx, vpp_gpe_ctx);
   
    BEGIN_BATCH(vpp_gpe_ctx->batch, 2);
    OUT_BATCH(vpp_gpe_ctx->batch, MI_BATCH_BUFFER_START | (2 << 6));
    OUT_RELOC(vpp_gpe_ctx->batch,
              vpp_gpe_ctx->vpp_batchbuffer.bo,
              I915_GEM_DOMAIN_COMMAND, 0, 
              0);
    ADVANCE_BATCH(vpp_gpe_ctx->batch);

    intel_batchbuffer_end_atomic(vpp_gpe_ctx->batch);
	
    return VA_STATUS_SUCCESS;
}

static VAStatus
gpe_process_init(VADriverContextP ctx,
                 struct vpp_gpe_context *vpp_gpe_ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    dri_bo *bo;

    unsigned int batch_buf_size = vpp_gpe_ctx->thread_num * 
                 (vpp_gpe_ctx->thread_param_size + 6 * sizeof(int)) + 16;

    vpp_gpe_ctx->vpp_kernel_return.num_blocks = vpp_gpe_ctx->thread_num;
    vpp_gpe_ctx->vpp_kernel_return.size_block = 16;
    vpp_gpe_ctx->vpp_kernel_return.pitch = 1;
    unsigned int kernel_return_size =  vpp_gpe_ctx->vpp_kernel_return.num_blocks   
           * vpp_gpe_ctx->vpp_kernel_return.size_block;
 
    dri_bo_unreference(vpp_gpe_ctx->vpp_batchbuffer.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "vpp batch buffer",
                       batch_buf_size, 0x1000);
    vpp_gpe_ctx->vpp_batchbuffer.bo = bo;
    dri_bo_reference(vpp_gpe_ctx->vpp_batchbuffer.bo);

    dri_bo_unreference(vpp_gpe_ctx->vpp_kernel_return.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "vpp kernel return buffer",
                       kernel_return_size, 0x1000);
    vpp_gpe_ctx->vpp_kernel_return.bo = bo;
    dri_bo_reference(vpp_gpe_ctx->vpp_kernel_return.bo); 

    i965_gpe_context_init(ctx, &vpp_gpe_ctx->gpe_ctx);

    return VA_STATUS_SUCCESS;
}

static VAStatus
gpe_process_prepare(VADriverContextP ctx, 
                    struct vpp_gpe_context *vpp_gpe_ctx)
{
    /*Setup all the memory object*/
    gpe_surfaces_setup(ctx, vpp_gpe_ctx);
    gpe_interface_setup(ctx, vpp_gpe_ctx);
    gpe_constant_setup(ctx, vpp_gpe_ctx);

    /*Programing media pipeline*/
    gpe_pipeline_setup(ctx, vpp_gpe_ctx);
	
    return VA_STATUS_SUCCESS;
}

static VAStatus
gpe_process_run(VADriverContextP ctx, 
                struct vpp_gpe_context *vpp_gpe_ctx)
{
    intel_batchbuffer_flush(vpp_gpe_ctx->batch);
    
    return VA_STATUS_SUCCESS;
}

static VAStatus
gen75_gpe_process(VADriverContextP ctx, 
                  struct vpp_gpe_context * vpp_gpe_ctx)
{
    VAStatus va_status = VA_STATUS_SUCCESS;
    va_status = gpe_process_init(ctx, vpp_gpe_ctx);
    va_status |=gpe_process_prepare(ctx, vpp_gpe_ctx);
    va_status |=gpe_process_run(ctx, vpp_gpe_ctx);
 
    return va_status;
}

static VAStatus
gen75_gpe_process_sharpening(VADriverContextP ctx,
                             struct vpp_gpe_context * vpp_gpe_ctx)
{
     VAStatus va_status = VA_STATUS_SUCCESS;
     struct i965_driver_data *i965 = i965_driver_data(ctx);
     VASurfaceID origin_in_surf_id = vpp_gpe_ctx-> surface_input[0];
     VASurfaceID origin_out_surf_id = vpp_gpe_ctx-> surface_output;

     VAProcPipelineParameterBuffer* pipe = vpp_gpe_ctx->pipeline_param;
     VABufferID *filter_ids = (VABufferID*)pipe->filters ;
     struct object_buffer *obj_buf = BUFFER((*(filter_ids + 0)));
     VAProcFilterParameterBuffer* filter =
                  (VAProcFilterParameterBuffer*)obj_buf-> buffer_store->buffer;
     float sharpening_intensity = filter->value;

     ThreadParameterSharpening thr_param;
     unsigned int thr_param_size = sizeof(ThreadParameterSharpening);
     unsigned int i;
     unsigned char * pos;

     if(vpp_gpe_ctx->is_first_frame){
         vpp_gpe_ctx->sub_shader_sum = 3;
         i965_gpe_load_kernels(ctx,
                               &vpp_gpe_ctx->gpe_ctx,
                               gen75_vpp_sharpening_kernels,
                               vpp_gpe_ctx->sub_shader_sum);
     }

     if(!vpp_gpe_ctx->surface_tmp){
        va_status = i965_CreateSurfaces(ctx,
                                       vpp_gpe_ctx->in_frame_w,
                                       vpp_gpe_ctx->in_frame_h,
                                       VA_RT_FORMAT_YUV420,
                                       1,
                                       &vpp_gpe_ctx->surface_tmp);
       assert(va_status == VA_STATUS_SUCCESS);
    
       struct object_surface * obj_surf = SURFACE(vpp_gpe_ctx->surface_tmp);
       i965_check_alloc_surface_bo(ctx, obj_surf, 1, VA_FOURCC('N','V','1','2'),
                                   SUBSAMPLE_YUV420);
    }                

    assert(sharpening_intensity >= 0.0 && sharpening_intensity <= 1.0);
    thr_param.l_amount = (unsigned int)(sharpening_intensity * 128);
    thr_param.d_amount = (unsigned int)(sharpening_intensity * 128);

    thr_param.base.pic_width = vpp_gpe_ctx->in_frame_w;
    thr_param.base.pic_height = vpp_gpe_ctx->in_frame_h;

    /* Step 1: horizontal blur process */      
    vpp_gpe_ctx->forward_surf_sum = 0;
    vpp_gpe_ctx->backward_surf_sum = 0;
 
    vpp_gpe_ctx->thread_num = vpp_gpe_ctx->in_frame_h/16;
    vpp_gpe_ctx->thread_param_size = thr_param_size;
    vpp_gpe_ctx->thread_param = (unsigned char*) malloc(vpp_gpe_ctx->thread_param_size
                                                       *vpp_gpe_ctx->thread_num);
    pos = vpp_gpe_ctx->thread_param;
    for( i = 0 ; i < vpp_gpe_ctx->thread_num; i++){
        thr_param.base.v_pos = 16 * i;
        thr_param.base.h_pos = 0;
        memcpy(pos, &thr_param, thr_param_size);
        pos += thr_param_size;
    }

    vpp_gpe_ctx->sub_shader_index = 0;
    va_status = gen75_gpe_process(ctx, vpp_gpe_ctx);
    free(vpp_gpe_ctx->thread_param);

    /* Step 2: vertical blur process */      
    vpp_gpe_ctx->surface_input[0] = vpp_gpe_ctx->surface_output;
    vpp_gpe_ctx->surface_output = vpp_gpe_ctx->surface_tmp;
    vpp_gpe_ctx->forward_surf_sum = 0;
    vpp_gpe_ctx->backward_surf_sum = 0;
 
    vpp_gpe_ctx->thread_num = vpp_gpe_ctx->in_frame_w/16;
    vpp_gpe_ctx->thread_param_size = thr_param_size;
    vpp_gpe_ctx->thread_param = (unsigned char*) malloc(vpp_gpe_ctx->thread_param_size
                                                       *vpp_gpe_ctx->thread_num);
    pos = vpp_gpe_ctx->thread_param;
    for( i = 0 ; i < vpp_gpe_ctx->thread_num; i++){
        thr_param.base.v_pos = 0;
        thr_param.base.h_pos = 16 * i;
        memcpy(pos, &thr_param, thr_param_size);
        pos += thr_param_size;
    }

    vpp_gpe_ctx->sub_shader_index = 1;
    gen75_gpe_process(ctx, vpp_gpe_ctx);
    free(vpp_gpe_ctx->thread_param);

    /* Step 3: apply the blur to original surface */      
    vpp_gpe_ctx->surface_input[0]  = origin_in_surf_id;
    vpp_gpe_ctx->surface_input[1]  = vpp_gpe_ctx->surface_tmp;
    vpp_gpe_ctx->surface_output    = origin_out_surf_id;
    vpp_gpe_ctx->forward_surf_sum  = 1;
    vpp_gpe_ctx->backward_surf_sum = 0;
 
    vpp_gpe_ctx->thread_num = vpp_gpe_ctx->in_frame_h/4;
    vpp_gpe_ctx->thread_param_size = thr_param_size;
    vpp_gpe_ctx->thread_param = (unsigned char*) malloc(vpp_gpe_ctx->thread_param_size
                                                       *vpp_gpe_ctx->thread_num);
    pos = vpp_gpe_ctx->thread_param;
    for( i = 0 ; i < vpp_gpe_ctx->thread_num; i++){
        thr_param.base.v_pos = 4 * i;
        thr_param.base.h_pos = 0;
        memcpy(pos, &thr_param, thr_param_size);
        pos += thr_param_size;
    }

    vpp_gpe_ctx->sub_shader_index = 2;
    va_status = gen75_gpe_process(ctx, vpp_gpe_ctx);
    free(vpp_gpe_ctx->thread_param);

    return va_status;
}

VAStatus gen75_gpe_process_picture(VADriverContextP ctx, 
                    struct vpp_gpe_context * vpp_gpe_ctx)
{
    VAStatus va_status = VA_STATUS_SUCCESS;
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAProcPipelineParameterBuffer* pipe = vpp_gpe_ctx->pipeline_param;
    VAProcFilterParameterBuffer* filter = NULL;
    unsigned int i;

    for(i = 0; i < pipe->num_filters; i++){
        struct object_buffer *obj_buf = BUFFER(pipe->filters[i]);
        filter = (VAProcFilterParameterBuffer*)obj_buf-> buffer_store->buffer;
        if(filter->type == VAProcFilterSharpening){
           break;
        }
    }
       
    assert(pipe->num_forward_references + pipe->num_backward_references <= 4);
    vpp_gpe_ctx->surface_input[0] = pipe->surface;

    vpp_gpe_ctx->forward_surf_sum = 0;
    vpp_gpe_ctx->backward_surf_sum = 0;
 
    for(i = 0; i < pipe->num_forward_references; i ++)
    {
        vpp_gpe_ctx->surface_input[i + 1] = pipe->forward_references[i]; 
        vpp_gpe_ctx->forward_surf_sum ++;
    } 

    for(i = 0; i < pipe->num_backward_references; i ++)
    {
         vpp_gpe_ctx->surface_input[vpp_gpe_ctx->forward_surf_sum + 1 + i ] = 
                                    pipe->backward_references[i]; 
         vpp_gpe_ctx->backward_surf_sum ++;
    } 

    struct object_surface *obj_surface = SURFACE(vpp_gpe_ctx->surface_input[0]);
    vpp_gpe_ctx->in_frame_w = obj_surface->orig_width;
    vpp_gpe_ctx->in_frame_h = obj_surface->orig_height;

    if(filter->type == VAProcFilterSharpening) {
       va_status = gen75_gpe_process_sharpening(ctx, vpp_gpe_ctx); 
    } else {
       va_status = VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    }

    vpp_gpe_ctx->is_first_frame = 0;

    return va_status;
}

void 
gen75_gpe_context_destroy(VADriverContextP ctx, 
                               struct vpp_gpe_context *vpp_gpe_ctx)
{
    dri_bo_unreference(vpp_gpe_ctx->vpp_batchbuffer.bo);
    vpp_gpe_ctx->vpp_batchbuffer.bo = NULL;

    dri_bo_unreference(vpp_gpe_ctx->vpp_kernel_return.bo);
    vpp_gpe_ctx->vpp_kernel_return.bo = NULL;

    i965_gpe_context_destroy(&vpp_gpe_ctx->gpe_ctx);

    if(vpp_gpe_ctx->surface_tmp){
       i965_DestroySurfaces(ctx, &vpp_gpe_ctx->surface_tmp, 1);
       vpp_gpe_ctx->surface_tmp = NULL;
    }   

    free(vpp_gpe_ctx->batch);

    free(vpp_gpe_ctx);
}

struct hw_context * 
gen75_gpe_context_init(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct vpp_gpe_context  *vpp_gpe_ctx = calloc(1, sizeof(struct vpp_gpe_context));
    struct i965_gpe_context *gpe_ctx = &(vpp_gpe_ctx->gpe_ctx);

    gpe_ctx->surface_state_binding_table.length = 
               (SURFACE_STATE_PADDED_SIZE + sizeof(unsigned int)) * MAX_MEDIA_SURFACES_GEN6;
    gpe_ctx->idrt.max_entries = MAX_INTERFACE_DESC_GEN6;
    gpe_ctx->idrt.entry_size = sizeof(struct gen6_interface_descriptor_data);

    gpe_ctx->curbe.length = CURBE_TOTAL_DATA_LENGTH;

    gpe_ctx->vfe_state.max_num_threads = 60 - 1;
    gpe_ctx->vfe_state.num_urb_entries = 16;
    gpe_ctx->vfe_state.gpgpu_mode = 0;
    gpe_ctx->vfe_state.urb_entry_size = 59 - 1;
    gpe_ctx->vfe_state.curbe_allocation_size = CURBE_ALLOCATION_SIZE - 1;
 
    vpp_gpe_ctx->vpp_surface2_setup             = gen7_gpe_surface2_setup;
    vpp_gpe_ctx->vpp_media_rw_surface_setup     = gen7_gpe_media_rw_surface_setup;
    vpp_gpe_ctx->vpp_buffer_surface_setup       = gen7_gpe_buffer_suface_setup;
    vpp_gpe_ctx->vpp_media_chroma_surface_setup = gen75_gpe_media_chroma_surface_setup;

    vpp_gpe_ctx->batch = intel_batchbuffer_new(&i965->intel, I915_EXEC_RENDER, 0);

    vpp_gpe_ctx->is_first_frame = 1; 

    return (struct hw_context *)vpp_gpe_ctx;
}

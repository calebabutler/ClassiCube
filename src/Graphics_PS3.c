#include "Core.h"
#if defined CC_BUILD_PS3
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"
#include <malloc.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

/* Current format and size of vertices */
static int gfx_stride, gfx_format = -1;
static cc_bool renderingDisabled;
static gcmContextData* context;
static u32 cur_fb = 0;

#define CB_SIZE   0x100000 // TODO: smaller command buffer?
#define HOST_SIZE (32 * 1024 * 1024)


/*########################################################################################################################*
*----------------------------------------------------- Vertex Shaders ----------------------------------------------------*
*#########################################################################################################################*/
typedef struct CCVertexProgram {
	rsxVertexProgram* prog;
	void* ucode;
	rsxProgramConst* mvp;
} VertexProgram;

extern const u8 vs_textured_vpo[];
extern const u8 vs_coloured_vpo[];

static VertexProgram  VP_list[2];
static VertexProgram* VP_active;


static void VP_Load(VertexProgram* vp, const u8* source) {
	vp->prog = (rsxVertexProgram*)source;
	u32 size = 0;
	rsxVertexProgramGetUCode(vp->prog, &vp->ucode, &size);
	
	vp->mvp = rsxVertexProgramGetConst(vp->prog, "mvp");
}

static void LoadVertexPrograms(void) {
	VP_Load(&VP_list[0], vs_coloured_vpo);
	VP_Load(&VP_list[1], vs_textured_vpo);
}

static void VP_SwitchActive(void) {
	int index = gfx_format == VERTEX_FORMAT_TEXTURED ? 1 : 0;
	
	VertexProgram* VP = &VP_list[index];
	if (VP == VP_active) return;
	VP_active = VP;
	
	rsxLoadVertexProgram(context, VP->prog, VP->ucode);
}


/*########################################################################################################################*
*---------------------------------------------------- Fragment Shaders ---------------------------------------------------*
*#########################################################################################################################*/
typedef struct CCFragmentProgram {
	rsxFragmentProgram* prog;
	void* ucode;
	u32* buffer;
	u32 offset;
} FragmentProgram;

extern const u8 ps_textured_fpo[];
extern const u8 ps_coloured_fpo[];

static FragmentProgram  FP_list[2];
static FragmentProgram* FP_active;


static void FP_Load(FragmentProgram* fp, const u8* source) {
	fp->prog = (rsxFragmentProgram*)source;
	u32 size = 0;
	rsxFragmentProgramGetUCode(fp->prog, &fp->ucode, &size);
	
	fp->buffer = (u32*)rsxMemalign(128, size);
	Mem_Copy(fp->buffer, fp->ucode, size);
	rsxAddressToOffset(fp->buffer, &fp->offset);
}

static void LoadFragmentPrograms(void) {
	FP_Load(&FP_list[0], ps_textured_fpo);
	FP_Load(&FP_list[1], ps_coloured_fpo);
}

static void FP_SwitchActive(void) {
	int index = gfx_format == VERTEX_FORMAT_TEXTURED ? 1 : 0;
	
	FragmentProgram* FP = &FP_list[index];
	if (FP == FP_active) return;
	FP_active = FP;
	
	rsxLoadFragmentProgramLocation(context, FP->prog, FP->offset, GCM_LOCATION_RSX);
}


/*########################################################################################################################*
*---------------------------------------------------------- Setup---------------------------------------------------------*
*#########################################################################################################################*/
static u32  color_pitch;
static u32  color_offset[2];
static u32* color_buffer[2];

static u32  depth_pitch;
static u32  depth_offset;
static u32* depth_buffer;

static void Gfx_FreeState(void) { FreeDefaultResources(); }
static void Gfx_RestoreState(void) {
	InitDefaultResources();
	gfx_format = -1;/* TODO */
	
	rsxSetColorMaskMrt(context, 0);
	rsxSetDepthFunc(context, GCM_LEQUAL);
	rsxSetClearDepthStencil(context, 0xFFFFFFFF);
	//rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
	
	rsxSetUserClipPlaneControl(context,GCM_USER_CLIP_PLANE_DISABLE,
									   GCM_USER_CLIP_PLANE_DISABLE,
									   GCM_USER_CLIP_PLANE_DISABLE,
									   GCM_USER_CLIP_PLANE_DISABLE,
									   GCM_USER_CLIP_PLANE_DISABLE,
									   GCM_USER_CLIP_PLANE_DISABLE);
}

static void CreateContext(void) {
	void* host_addr = memalign(1024 * 1024, HOST_SIZE);
	rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);
}

static void ConfigureVideo(void) {
	videoState state;
	videoGetState(0, 0, &state);

	videoConfiguration vconfig = { 0 };
	vconfig.resolution = state.displayMode.resolution;
	vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch      = DisplayInfo.Width * sizeof(u32);
	
	videoConfigure(0, &vconfig, NULL, 0);
}

static void SetupBlendingState(void) {
	rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
}

static void AllocColorSurface(u32 i) {
	color_pitch     = DisplayInfo.Width * 4;
	color_buffer[i] = (u32*)rsxMemalign(64, DisplayInfo.Height * color_pitch);
	
	rsxAddressToOffset(color_buffer[i], &color_offset[i]);
	gcmSetDisplayBuffer(i, color_offset[i], color_pitch,
		DisplayInfo.Width, DisplayInfo.Height);
}

static void AllocDepthSurface(void) {
	depth_pitch  = DisplayInfo.Width * 4;
	depth_buffer = (u32*)rsxMemalign(64, DisplayInfo.Height * depth_pitch);
	
	rsxAddressToOffset(depth_buffer, &depth_offset);
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
void SetRenderTarget(u32 index) {
	gcmSurface sf;

	sf.colorFormat		= GCM_SURFACE_X8R8G8B8;
	sf.colorTarget		= GCM_SURFACE_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index];
	sf.colorPitch[0]	= color_pitch;

	sf.colorLocation[1]	= GCM_LOCATION_RSX;
	sf.colorLocation[2]	= GCM_LOCATION_RSX;
	sf.colorLocation[3]	= GCM_LOCATION_RSX;
	sf.colorOffset[1]	= 0;
	sf.colorOffset[2]	= 0;
	sf.colorOffset[3]	= 0;
	sf.colorPitch[1]	= 64;
	sf.colorPitch[2]	= 64;
	sf.colorPitch[3]	= 64;

	sf.depthFormat		= GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation	= GCM_LOCATION_RSX;
	sf.depthOffset		= depth_offset;
	sf.depthPitch		= depth_pitch;

	sf.type		= GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias		= GCM_SURFACE_CENTER_1;

	sf.width		= DisplayInfo.Width;
	sf.height		= DisplayInfo.Height;
	sf.x			= 0;
	sf.y			= 0;

	rsxSetSurface(context,&sf);
}
static GfxResourceID white_square;

void Gfx_Create(void) {
	// TODO rethink all this
	if (Gfx.Created) return;
	Gfx.MaxTexWidth  = 1024;
	Gfx.MaxTexHeight = 1024;
	Gfx.Created      = true;
	
	// https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/rsx/rsx.h#L30
	CreateContext();
	ConfigureVideo();
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	
	AllocColorSurface(0);
	AllocColorSurface(1);
	AllocDepthSurface();
	gcmResetFlipStatus();
	
	SetupBlendingState();
	Gfx_RestoreState();
	SetRenderTarget(cur_fb);
	
	LoadVertexPrograms();
	LoadFragmentPrograms();
	
	// 1x1 dummy white texture
	struct Bitmap bmp;
	BitmapCol pixels[1] = { BITMAPCOLOR_WHITE };
	Bitmap_Init(bmp, 1, 1, pixels);
	white_square = Gfx_CreateTexture(&bmp, 0, false);
}

cc_bool Gfx_TryRestoreContext(void) { return true; }

cc_bool Gfx_WarnIfNecessary(void) { return false; }

void Gfx_Free(void) {
	Gfx_FreeState();
}

u32* Gfx_AllocImage(u32* offset, s32 w, s32 h) {
	u32* pixels = (u32*)rsxMemalign(64, w * h * 4);
	rsxAddressToOffset(pixels, offset);
	return pixels;
}

void Gfx_TransferImage(u32 offset, s32 w, s32 h) {
	rsxSetTransferImage(context, GCM_TRANSFER_LOCAL_TO_LOCAL,
		color_offset[cur_fb], color_pitch, 0, 0,
		offset, w * 4, 0, 0, 
		w, h, 4);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
static PackedCol gfx_clearColor;
void Gfx_SetFaceCulling(cc_bool enabled) {
	rsxSetCullFaceEnable(context, enabled);
}

void Gfx_SetAlphaBlending(cc_bool enabled) {
	rsxSetBlendEnable(context, enabled);
}
void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

void Gfx_ClearCol(PackedCol color) {
        rsxSetClearColor(context, color);
}

void Gfx_SetColWriteMask(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	unsigned mask = 0;
	if (r) mask |= GCM_COLOR_MASK_R;
	if (g) mask |= GCM_COLOR_MASK_G;
	if (b) mask |= GCM_COLOR_MASK_B;
	if (a) mask |= GCM_COLOR_MASK_A;

	rsxSetColorMask(context, mask);
}

void Gfx_SetDepthWrite(cc_bool enabled) {
	rsxSetDepthWriteEnable(context, enabled);
}

void Gfx_SetDepthTest(cc_bool enabled) {
	rsxSetDepthTestEnable(context, enabled);
}

void Gfx_SetTexturing(cc_bool enabled) { }

void Gfx_SetAlphaTest(cc_bool enabled) { /* TODO */ }

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {/* TODO */
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	// Same as Direct3D9
	*matrix = Matrix_Identity;

	matrix->row1.X =  2.0f / width;
	matrix->row2.Y = -2.0f / height;
	matrix->row3.Z =  1.0f / (zNear - zFar);

	matrix->row4.X = -1.0f;
	matrix->row4.Y =  1.0f;
	matrix->row4.Z = zNear / (zNear - zFar);
}

static double Cotangent(double x) { return Math_Cos(x) / Math_Sin(x); }
void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	float zNear = 0.1f;
	float c = (float)Cotangent(0.5f * fov);

	// Same as Direct3D9
	*matrix = Matrix_Identity;

	matrix->row1.X =  c / aspect;
	matrix->row2.Y =  c;
	matrix->row3.Z = zFar / (zNear - zFar);
	matrix->row3.W = -1.0f;
	matrix->row4.Z = (zNear * zFar) / (zNear - zFar);
	matrix->row4.W =  0.0f;
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
cc_result Gfx_TakeScreenshot(struct Stream* output) {
	return ERR_NOT_SUPPORTED;
}

void Gfx_GetApiInfo(cc_string* info) {
	int pointerSize = sizeof(void*) * 8;

	String_Format1(info, "-- Using PS3 (%i bit) --\n", &pointerSize);
	String_Format2(info, "Max texture size: (%i, %i)\n", &Gfx.MaxTexWidth, &Gfx.MaxTexHeight);
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	gfx_minFrameMs = minFrameMs;
	gfx_vsync      = vsync;
}


// https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/rsx/rsx.h#L30
static cc_bool everFlipped;
void Gfx_BeginFrame(void) {
	// TODO: remove everFlipped
	if (everFlipped) {
		while (gcmGetFlipStatus() != 0) usleep(200);
	}
	
	everFlipped = true;
	gcmResetFlipStatus();
}

void Gfx_Clear(void) {
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A 
		| GCM_CLEAR_S | GCM_CLEAR_Z);
}

void Gfx_EndFrame(void) {
	gcmSetFlip(context, cur_fb);
	rsxFlushBuffer(context);
	gcmSetWaitFlip(context);

	cur_fb ^= 1;
	SetRenderTarget(cur_fb);
	
	if (gfx_minFrameMs) LimitFPS();
}

void Gfx_OnWindowResize(void) {
	f32 scale[4], offset[4];

	u16 w = DisplayInfo.Width;
	u16 h = DisplayInfo.Height;
	f32 zmin = 0.0f;
	f32 zmax = 1.0f;
	
	scale[0]  = w * 0.5f;
	scale[1]  = h * -0.5f;
	scale[2]  = (zmax - zmin) * 0.5f;
	scale[3]  = 0.0f;
	offset[0] = w * 0.5f;
	offset[1] = h * 0.5f;
	offset[2] = (zmax + zmin) * 0.5f;
	offset[3] = 0.0f;

	rsxSetViewport(context, 0, 0, w, h, zmin, zmax, scale, offset);
	rsxSetScissor(context, 0, 0, w, h);
	
	// TODO: even needed?
	for (int i = 0; i < 8; i++)
	{
		rsxSetViewportClip(context, i, w, h);
	}
	/* TODO test */
}


/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
static int vb_size;

GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
	return 1;/* TODO */
}

void Gfx_BindIb(GfxResourceID ib) { }
void Gfx_DeleteIb(GfxResourceID* ib) { }


/*########################################################################################################################*
*------------------------------------------------------Vertex buffers-----------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateVb(VertexFormat fmt, int count) {
	void* data = rsxMemalign(128, count * strideSizes[fmt]);
	if (!data) Logger_Abort("Failed to allocate memory for GFX VB");
	return data;
}

void Gfx_BindVb(GfxResourceID vb) { 
	u32 offset;
	rsxAddressToOffset(vb, &offset);
	
	if (gfx_format == VERTEX_FORMAT_TEXTURED) {
		rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS,    0, offset, 
			SIZEOF_VERTEX_TEXTURED, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0, offset + 12, 
			SIZEOF_VERTEX_TEXTURED, 4, GCM_VERTEX_DATA_TYPE_U8,  GCM_LOCATION_RSX);
		rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0,   0, offset + 16,
			SIZEOF_VERTEX_TEXTURED, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	} else {
		rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS,    0, offset, 
			SIZEOF_VERTEX_COLOURED, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0, offset + 12, 
			SIZEOF_VERTEX_COLOURED, 4, GCM_VERTEX_DATA_TYPE_U8,  GCM_LOCATION_RSX);
	}
}

void Gfx_DeleteVb(GfxResourceID* vb) {
	GfxResourceID data = *vb;/* TODO */
	if (data) rsxFree(data);
	*vb = 0;
}

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	vb_size = count * strideSizes[fmt];
	return vb;
}

void Gfx_UnlockVb(GfxResourceID vb) { 
	Gfx_BindVb(vb);
	rsxInvalidateVertexCache(context); // TODO needed?
}


GfxResourceID Gfx_CreateDynamicVb(VertexFormat fmt, int maxVertices) {
	void* data = rsxMemalign(128, maxVertices * strideSizes[fmt]);
	if (!data) Logger_Abort("Failed to allocate memory for GFX VB");
	return data;
}

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	vb_size = count * strideSizes[fmt];
	return vb;
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	Gfx_BindVb(vb);
	rsxInvalidateVertexCache(context); // TODO needed?
}

void Gfx_SetDynamicVbData(GfxResourceID vb, void* vertices, int vCount) {
	Mem_Copy(vb, vertices, vCount * gfx_stride);
	Gfx_BindVb(vb);
	rsxInvalidateVertexCache(context); // TODO needed?
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
typedef struct CCTexture_ {
	cc_uint32 width, height;
	cc_uint32 pad[(128 - 8)/4]; // TODO better way of aligning to 128 bytes
	cc_uint32 pixels[];
} CCTexture;

GfxResourceID Gfx_CreateTexture(struct Bitmap* bmp, cc_uint8 flags, cc_bool mipmaps) {
	int size = bmp->width * bmp->height * 4;
	CCTexture* tex = (CCTexture*)rsxMemalign(128, 128 + size);
	
	tex->width  = bmp->width;
	tex->height = bmp->height;
	Mem_Copy(tex->pixels, bmp->scan0, size);
	return tex;
}

void Gfx_BindTexture(GfxResourceID texId) {
	CCTexture* tex = (CCTexture*)texId;
	if (!tex) tex  = white_square; 
	/* TODO */
	
	u32 offset;
	rsxAddressToOffset(tex->pixels, &offset);
	gcmTexture texture;

	texture.format		= GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap		= 1;
	texture.dimension	= GCM_TEXTURE_DIMS_2D;
	texture.cubemap		= GCM_FALSE;
	texture.remap		= ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
						   (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
						   (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
						   (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
						   (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
						   (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
						   (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
						   (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	texture.width		= tex->width;
	texture.height		= tex->height;
	texture.depth		= 1;
	texture.location	= GCM_LOCATION_RSX;
	texture.pitch		= tex->width * 4;
	texture.offset		= offset;
	
	rsxInvalidateTextureCache(context,GCM_INVALIDATE_TEXTURE); // TODO needed
	
	rsxLoadTexture(context,    0, &texture);
	rsxTextureControl(context, 0, GCM_TRUE, 0<<8, 12<<8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, 0, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST,
		GCM_TEXTURE_CONVOLUTION_QUINCUNX);			
	rsxTextureWrapMode(context, 0, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	GfxResourceID data = *texId;
	if (data) rsxFree(data);
	*texId = NULL;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	/* TODO */
}

void Gfx_UpdateTexturePart(GfxResourceID texId, int x, int y, struct Bitmap* part, cc_bool mipmaps) {
	Gfx_UpdateTexture(texId, x, y, part, part->width, mipmaps);
}

void Gfx_EnableMipmaps(void)  { }
void Gfx_DisableMipmaps(void) { }


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFog(cc_bool enabled) {/* TODO */
}

void Gfx_SetFogCol(PackedCol color) {/* TODO */
}

void Gfx_SetFogDensity(float value) {/* TODO */
}

void Gfx_SetFogEnd(float value) {/* TODO */
}

void Gfx_SetFogMode(FogFunc func) {/* TODO */
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
static struct Matrix _view, _proj;

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	struct Matrix* dst = type == MATRIX_PROJECTION ? &_proj : &_view;
	*dst = *matrix;
	
	struct Matrix mvp;
	Matrix_Mul(&mvp, &_view, &_proj);
	
	// TODO: dirty uniforms instead
	for (int i = 0; i < Array_Elems(VP_list); i++)
	{
		VertexProgram* vp = &VP_list[i];
		rsxSetVertexProgramParameter(context, vp->prog, vp->mvp, (float*)&mvp);
	}
}

void Gfx_LoadIdentityMatrix(MatrixType type) {
	Gfx_LoadMatrix(type, &Matrix_Identity);
}

void Gfx_EnableTextureOffset(float x, float y) {
/* TODO */
}

void Gfx_DisableTextureOffset(void) {
/* TODO */
}


/*########################################################################################################################*
*----------------------------------------------------------Drawing--------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];/* TODO */
	
	VP_SwitchActive();
	FP_SwitchActive();
}

void Gfx_DrawVb_Lines(int verticesCount) {/* TODO */
	rsxDrawVertexArray(context, GCM_TYPE_LINES, 0, verticesCount);
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {/* TODO */
	rsxDrawVertexArray(context, GCM_TYPE_QUADS, startVertex, verticesCount);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {/* TODO */
	rsxDrawVertexArray(context, GCM_TYPE_QUADS, 0, verticesCount);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {/* TODO */
	rsxDrawVertexArray(context, GCM_TYPE_QUADS, startVertex, verticesCount);
}
#endif
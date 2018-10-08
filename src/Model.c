#include "Model.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "ModelCache.h"
#include "GraphicsCommon.h"
#include "GraphicsAPI.h"
#include "Entity.h"

#define UV_POS_MASK ((uint16_t)0x7FFF)
#define UV_MAX ((uint16_t)0x8000)
#define UV_MAX_SHIFT 15
#define AABB_Width(bb)  (bb->Max.X - bb->Min.X)
#define AABB_Height(bb) (bb->Max.Y - bb->Min.Y)
#define AABB_Length(bb) (bb->Max.Z - bb->Min.Z)

void ModelVertex_Init(struct ModelVertex* vertex, float x, float y, float z, int u, int v) {
	vertex->X = x; vertex->Y = y; vertex->Z = z;
	vertex->U = (uint16_t)u; vertex->V = (uint16_t)v;
}

void ModelPart_Init(struct ModelPart* part, int offset, int count, float rotX, float rotY, float rotZ) {
	part->Offset = offset; part->Count = count;
	part->RotX = rotX; part->RotY = rotY; part->RotZ = rotZ;
}


/*########################################################################################################################*
*------------------------------------------------------------Model--------------------------------------------------------*
*#########################################################################################################################*/
static void Model_GetTransform(struct Entity* entity, Vector3 pos, struct Matrix* m) {
	Entity_GetTransform(entity, pos, entity->ModelScale, m);
}
static void Model_NullFunc(struct Entity* entity) { }

void Model_Init(struct Model* model) {
	model->Bobbing  = true;
	model->UsesSkin = true;
	model->CalcHumanAnims = false;
	model->UsesHumanSkin  = false;
	model->Pushes = true;

	model->Gravity = 0.08f;
	model->Drag = Vector3_Create3(0.91f, 0.98f, 0.91f);
	model->GroundFriction = Vector3_Create3(0.6f, 1.0f, 0.6f);

	model->MaxScale    = 2.0f;
	model->ShadowScale = 1.0f;
	model->NameScale   = 1.0f;
	model->armX = 6; model->armY = 12;

	model->GetTransform = Model_GetTransform;
	model->RecalcProperties = Model_NullFunc;
	model->DrawArm = Model_NullFunc;
}

bool Model_ShouldRender(struct Entity* entity) {
	Vector3 pos = entity->Position;
	struct AABB bb; Entity_GetPickingBounds(entity, &bb);

	struct AABB* bbPtr = &bb;
	float bbWidth  = AABB_Width(bbPtr);
	float bbHeight = AABB_Height(bbPtr);
	float bbLength = AABB_Length(bbPtr);

	float maxYZ  = max(bbHeight, bbLength);
	float maxXYZ = max(bbWidth, maxYZ);
	pos.Y += AABB_Height(bbPtr) * 0.5f; /* Centre Y coordinate. */
	return FrustumCulling_SphereInFrustum(pos.X, pos.Y, pos.Z, maxXYZ);
}

static float Model_MinDist(float dist, float extent) {
	/* Compare min coord, centre coord, and max coord */
	float dMin = Math_AbsF(dist - extent), dMax = Math_AbsF(dist + extent);
	float dMinMax = min(dMin, dMax);
	return min(Math_AbsF(dist), dMinMax);
}

float Model_RenderDistance(struct Entity* entity) {
	Vector3 pos = entity->Position;
	struct AABB* bb = &entity->ModelAABB;
	pos.Y += AABB_Height(bb) * 0.5f; /* Centre Y coordinate. */
	Vector3 camPos = Game_CurrentCameraPos;

	float dx = Model_MinDist(camPos.X - pos.X, AABB_Width(bb)  * 0.5f);
	float dy = Model_MinDist(camPos.Y - pos.Y, AABB_Height(bb) * 0.5f);
	float dz = Model_MinDist(camPos.Z - pos.Z, AABB_Length(bb) * 0.5f);
	return dx * dx + dy * dy + dz * dz;
}

struct Matrix Model_transform;
void Model_Render(struct Model* model, struct Entity* entity) {
	Vector3 pos = entity->Position;
	if (model->Bobbing) pos.Y += entity->Anim.BobbingModel;
	Model_SetupState(model, entity);
	Gfx_SetBatchFormat(VERTEX_FORMAT_P3FT2FC4B);

	model->GetTransform(entity, pos, &entity->Transform);
	struct Matrix m;
	Matrix_Mul(&m, &entity->Transform, &Gfx_View);

	Gfx_LoadMatrix(&m);
	model->DrawModel(entity);
	Gfx_LoadMatrix(&Gfx_View);
}

void Model_SetupState(struct Model* model, struct Entity* entity) {
	model->index = 0;
	PackedCol col = entity->VTABLE->GetCol(entity);

	bool _64x64 = entity->SkinType != SKIN_64x32;
	/* only apply when using humanoid skins */
	_64x64 &= model->UsesHumanSkin || entity->MobTextureId;

	Model_uScale = entity->uScale * 0.015625f;
	Model_vScale = entity->vScale * (_64x64 ? 0.015625f : 0.03125f);

	Model_Cols[0] = col;
	if (!entity->NoShade) {
		Model_Cols[1] = PackedCol_Scale(col, PACKEDCOL_SHADE_YMIN);
		Model_Cols[2] = PackedCol_Scale(col, PACKEDCOL_SHADE_Z);
		Model_Cols[4] = PackedCol_Scale(col, PACKEDCOL_SHADE_X);
	} else {
		Model_Cols[1] = col; Model_Cols[2] = col; Model_Cols[4] = col;
	}
	Model_Cols[3] = Model_Cols[2]; 
	Model_Cols[5] = Model_Cols[4];

	float yawDelta = entity->HeadY - entity->RotY;
	Model_cosHead = (float)Math_Cos(yawDelta * MATH_DEG2RAD);
	Model_sinHead = (float)Math_Sin(yawDelta * MATH_DEG2RAD);
	Model_ActiveModel = model;
}

void Model_UpdateVB(void) {
	struct Model* model = Model_ActiveModel;
	GfxCommon_UpdateDynamicVb_IndexedTris(ModelCache_Vb, ModelCache_Vertices, model->index);
	model->index = 0;
}

void Model_ApplyTexture(struct Entity* entity) {
	struct Model* model = Model_ActiveModel;
	GfxResourceID tex = model->UsesHumanSkin ? entity->TextureId : entity->MobTextureId;
	if (tex) {
		Model_skinType = entity->SkinType;
	} else {
		struct CachedTexture* data = &ModelCache_Textures[model->defaultTexIndex];
		tex = data->TexID;
		Model_skinType = data->SkinType;
	}

	Gfx_BindTexture(tex);
	bool _64x64  = Model_skinType != SKIN_64x32;
	Model_uScale = entity->uScale * 0.015625f;
	Model_vScale = entity->vScale * (_64x64 ? 0.015625f : 0.03125f);
}

void Model_DrawPart(struct ModelPart* part) {
	struct Model* model = Model_ActiveModel;
	struct ModelVertex* src = &model->vertices[part->Offset];
	VertexP3fT2fC4b* dst = &ModelCache_Vertices[model->index];
	int i, count = part->Count;

	for (i = 0; i < count; i++) {
		struct ModelVertex v = *src;
		dst->X = v.X; dst->Y = v.Y; dst->Z = v.Z;
		dst->Col = Model_Cols[i >> 2];

		dst->U = (v.U & UV_POS_MASK) * Model_uScale - (v.U >> UV_MAX_SHIFT) * 0.01f * Model_uScale;
		dst->V = (v.V & UV_POS_MASK) * Model_vScale - (v.V >> UV_MAX_SHIFT) * 0.01f * Model_vScale;
		src++; dst++;
	}
	model->index += count;
}

#define Model_RotateX t = cosX * v.Y + sinX * v.Z; v.Z = -sinX * v.Y + cosX * v.Z; v.Y = t;
#define Model_RotateY t = cosY * v.X - sinY * v.Z; v.Z = sinY * v.X + cosY * v.Z;  v.X = t;
#define Model_RotateZ t = cosZ * v.X + sinZ * v.Y; v.Y = -sinZ * v.X + cosZ * v.Y; v.X = t;

void Model_DrawRotate(float angleX, float angleY, float angleZ, struct ModelPart* part, bool head) {
	struct Model* model = Model_ActiveModel;
	float cosX = Math_CosF(-angleX), sinX = Math_SinF(-angleX);
	float cosY = Math_CosF(-angleY), sinY = Math_SinF(-angleY);
	float cosZ = Math_CosF(-angleZ), sinZ = Math_SinF(-angleZ);
	float x = part->RotX, y = part->RotY, z = part->RotZ;

	struct ModelVertex* src = &model->vertices[part->Offset];
	VertexP3fT2fC4b* dst = &ModelCache_Vertices[model->index];
	int i, count = part->Count;

	for (i = 0; i < count; i++) {
		struct ModelVertex v = *src;
		v.X -= x; v.Y -= y; v.Z -= z;
		float t = 0;

		/* Rotate locally */
		if (Model_Rotation == ROTATE_ORDER_ZYX) {
			Model_RotateZ
			Model_RotateY
			Model_RotateX
		} else if (Model_Rotation == ROTATE_ORDER_XZY) {
			Model_RotateX
			Model_RotateZ
			Model_RotateY
		} else if (Model_Rotation == ROTATE_ORDER_YZX) {
			Model_RotateY
			Model_RotateZ
			Model_RotateX
		}

		/* Rotate globally */
		if (head) {
			t = Model_cosHead * v.X - Model_sinHead * v.Z; v.Z = Model_sinHead * v.X + Model_cosHead * v.Z; v.X = t; /* Inlined RotY */
		}
		dst->X = v.X + x; dst->Y = v.Y + y; dst->Z = v.Z + z;
		dst->Col = Model_Cols[i >> 2];

		dst->U = (v.U & UV_POS_MASK) * Model_uScale - (v.U >> UV_MAX_SHIFT) * 0.01f * Model_uScale;
		dst->V = (v.V & UV_POS_MASK) * Model_vScale - (v.V >> UV_MAX_SHIFT) * 0.01f * Model_vScale;
		src++; dst++;
	}
	model->index += count;
}

void Model_RenderArm(struct Model* model, struct Entity* entity) {
	Vector3 pos = entity->Position;
	if (model->Bobbing) pos.Y += entity->Anim.BobbingModel;
	Model_SetupState(model, entity);

	Gfx_SetBatchFormat(VERTEX_FORMAT_P3FT2FC4B);
	Model_ApplyTexture(entity);
	struct Matrix translate;

	if (Game_ClassicArmModel) {
		// TODO: Position's not quite right.
		// Matrix4.Translate(out m, -armX / 16f + 0.2f, -armY / 16f - 0.20f, 0);
		// is better, but that breaks the dig animation
		Matrix_Translate(&translate, -model->armX / 16.0f,         -model->armY / 16.0f - 0.10f, 0);
	} else {
		Matrix_Translate(&translate, -model->armX / 16.0f + 0.10f, -model->armY / 16.0f - 0.26f, 0);
	}

	struct Matrix m; 
	Entity_GetTransform(entity, pos, entity->ModelScale, &m);
	Matrix_Mul(&m, &m,  &Gfx_View);
	Matrix_Mul(&m, &translate, &m);

	Gfx_LoadMatrix(&m);
	Model_Rotation = ROTATE_ORDER_YZX;
	model->DrawArm(entity);
	Model_Rotation = ROTATE_ORDER_ZYX;
	Gfx_LoadMatrix(&Gfx_View);
}

void Model_DrawArmPart(struct ModelPart* part) {
	struct Model* model = Model_ActiveModel;
	struct ModelPart arm = *part;
	arm.RotX = model->armX / 16.0f; 
	arm.RotY = (model->armY + model->armY / 2) / 16.0f;

	if (Game_ClassicArmModel) {
		Model_DrawRotate(0, -90 * MATH_DEG2RAD, 120 * MATH_DEG2RAD, &arm, false);
	} else {
		Model_DrawRotate(-20 * MATH_DEG2RAD, -70 * MATH_DEG2RAD, 135 * MATH_DEG2RAD, &arm, false);
	}
}


/*########################################################################################################################*
*----------------------------------------------------------BoxDesc--------------------------------------------------------*
*#########################################################################################################################*/
void BoxDesc_TexOrigin(struct BoxDesc* desc, int x, int y) {
	desc->TexX = x; desc->TexY = y;
}

void BoxDesc_Expand(struct BoxDesc* desc, float amount) {
	amount /= 16.0f;
	desc->X1 -= amount; desc->X2 += amount;
	desc->Y1 -= amount; desc->Y2 += amount;
	desc->Z1 -= amount; desc->Z2 += amount;
}

void BoxDesc_MirrorX(struct BoxDesc* desc) {
	float temp = desc->X1; desc->X1 = desc->X2; desc->X2 = temp;
}


void BoxDesc_BuildBox(struct ModelPart* part, struct BoxDesc* desc) {
	int sidesW = desc->SizeZ, bodyW = desc->SizeX, bodyH = desc->SizeY;
	float x1 = desc->X1, y1 = desc->Y1, z1 = desc->Z1;
	float x2 = desc->X2, y2 = desc->Y2, z2 = desc->Z2;
	int x = desc->TexX, y = desc->TexY;
	struct Model* m = Model_ActiveModel;

	BoxDesc_YQuad(m, x + sidesW,                  y,          bodyW, sidesW, x1, x2, z2, z1, y2, true);  /* top */
	BoxDesc_YQuad(m, x + sidesW + bodyW,          y,          bodyW, sidesW, x2, x1, z2, z1, y1, false); /* bottom */
	BoxDesc_ZQuad(m, x + sidesW,                  y + sidesW, bodyW,  bodyH, x1, x2, y1, y2, z1, true);  /* front */
	BoxDesc_ZQuad(m, x + sidesW + bodyW + sidesW, y + sidesW, bodyW,  bodyH, x2, x1, y1, y2, z2, true);  /* back */
	BoxDesc_XQuad(m, x,                           y + sidesW, sidesW, bodyH, z1, z2, y1, y2, x2, true);  /* left */
	BoxDesc_XQuad(m, x + sidesW + bodyW,          y + sidesW, sidesW, bodyH, z2, z1, y1, y2, x1, true);  /* right */

	ModelPart_Init(part, m->index - MODEL_BOX_VERTICES, MODEL_BOX_VERTICES,
		desc->RotX, desc->RotY, desc->RotZ);
}

void BoxDesc_BuildRotatedBox(struct ModelPart* part, struct BoxDesc* desc) {
	int sidesW = desc->SizeY, bodyW = desc->SizeX, bodyH = desc->SizeZ;
	float x1 = desc->X1, y1 = desc->Y1, z1 = desc->Z1;
	float x2 = desc->X2, y2 = desc->Y2, z2 = desc->Z2;
	int x = desc->TexX, y = desc->TexY;
	struct Model* m = Model_ActiveModel;

	BoxDesc_YQuad(m, x + sidesW + bodyW + sidesW, y + sidesW, bodyW,  bodyH, x1, x2, z1, z2, y2, false); /* top */
	BoxDesc_YQuad(m, x + sidesW,                  y + sidesW, bodyW,  bodyH, x2, x1, z1, z2, y1, false); /* bottom */
	BoxDesc_ZQuad(m, x + sidesW,                  y,          bodyW, sidesW, x2, x1, y1, y2, z1, false); /* front */
	BoxDesc_ZQuad(m, x + sidesW + bodyW,          y,          bodyW, sidesW, x1, x2, y2, y1, z2, false); /* back */
	BoxDesc_XQuad(m, x,                           y + sidesW, sidesW, bodyH, y2, y1, z2, z1, x2, false); /* left */
	BoxDesc_XQuad(m, x + sidesW + bodyW,          y + sidesW, sidesW, bodyH, y1, y2, z2, z1, x1, false); /* right */

	/* rotate left and right 90 degrees	*/
	int i;
	for (i = m->index - 8; i < m->index; i++) {
		struct ModelVertex vertex = m->vertices[i];
		float z = vertex.Z; vertex.Z = vertex.Y; vertex.Y = z;
		m->vertices[i] = vertex;
	}

	ModelPart_Init(part, m->index - MODEL_BOX_VERTICES, MODEL_BOX_VERTICES,
		desc->RotX, desc->RotY, desc->RotZ);
}


void BoxDesc_XQuad(struct Model* m, int texX, int texY, int texWidth, int texHeight, float z1, float z2, float y1, float y2, float x, bool swapU) {
	int u1 = texX, u2 = (texX + texWidth) | UV_MAX;
	if (swapU) { int tmp = u1; u1 = u2; u2 = tmp; }

	ModelVertex_Init(&m->vertices[m->index], x, y1, z1, u1, (texY + texHeight) | UV_MAX); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x, y2, z1, u1, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x, y2, z2, u2, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x, y1, z2, u2, (texY + texHeight) | UV_MAX); m->index++;
}

void BoxDesc_YQuad(struct Model* m, int texX, int texY, int texWidth, int texHeight, float x1, float x2, float z1, float z2, float y, bool swapU) {
	int u1 = texX, u2 = (texX + texWidth) | UV_MAX;
	if (swapU) { int tmp = u1; u1 = u2; u2 = tmp; }

	ModelVertex_Init(&m->vertices[m->index], x1, y, z2, u1, (texY + texHeight) | UV_MAX); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x1, y, z1, u1, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x2, y, z1, u2, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x2, y, z2, u2, (texY + texHeight) | UV_MAX); m->index++;
}

void BoxDesc_ZQuad(struct Model* m, int texX, int texY, int texWidth, int texHeight, float x1, float x2, float y1, float y2, float z, bool swapU) {
	int u1 = texX, u2 = (texX + texWidth) | UV_MAX;
	if (swapU) { int tmp = u1; u1 = u2; u2 = tmp; }

	ModelVertex_Init(&m->vertices[m->index], x1, y1, z, u1, (texY + texHeight) | UV_MAX); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x1, y2, z, u1, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x2, y2, z, u2, texY); m->index++;
	ModelVertex_Init(&m->vertices[m->index], x2, y1, z, u2, (texY + texHeight) | UV_MAX); m->index++;
}

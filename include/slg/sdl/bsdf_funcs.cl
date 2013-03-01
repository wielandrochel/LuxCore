#line 2 "bsdf_funcs.cl"

/***************************************************************************
 *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *
 *                                                                         *
 *   This file is part of LuxRays.                                         *
 *                                                                         *
 *   LuxRays is free software; you can redistribute it and/or modify       *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxRays is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 *                                                                         *
 *   LuxRays website: http://www.luxrender.net                             *
 ***************************************************************************/

void BSDF_Init(
		__global BSDF *bsdf,
		//const bool fromL,
#if defined(PARAM_ACCEL_MQBVH)
		__global uint *meshFirstTriangleOffset,
		__global Mesh *meshDescs,
#endif
		__global uint *meshMats,
		__global uint *meshIDs,
#if (PARAM_DL_LIGHT_COUNT > 0)
		__global uint *meshLights,
#endif
		__global Point *vertices,
		__global Vector *vertNormals,
		__global UV *vertUVs,
		__global Triangle *triangles,
		__global Ray *ray,
		__global RayHit *rayHit
#if defined(PARAM_HAS_PASSTHROUGH)
		, const float u0
#endif
#if defined(PARAM_HAS_BUMPMAPS) || defined(PARAM_HAS_NORMALMAPS)
		MATERIALS_PARAM_DECL
#endif
		) {
	//bsdf->fromLight = fromL;
#if defined(PARAM_HAS_PASSTHROUGH)
	bsdf->hitPoint.passThroughEvent = u0;
#endif

	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);
	const float3 hitPointP = rayOrig + rayHit->t * rayDir;
	VSTORE3F(hitPointP, &bsdf->hitPoint.p.x);
	VSTORE3F(-rayDir, &bsdf->hitPoint.fixedDir.x);

	const uint currentTriangleIndex = rayHit->index;
	const uint meshIndex = meshIDs[currentTriangleIndex];

#if defined(PARAM_ACCEL_MQBVH)
	__global Mesh *meshDesc = &meshDescs[meshIndex];
	__global Point *iVertices = &vertices[meshDesc->vertsOffset];
	__global Vector *iVertNormals = &vertNormals[meshDesc->vertsOffset];
	__global UV *iVertUVs = &vertUVs[meshDesc->vertsOffset];
	__global Triangle *iTriangles = &triangles[meshDesc->trisOffset];
	const uint triangleID = currentTriangleIndex - meshFirstTriangleOffset[meshIndex];
#endif

	// Get the material
	const uint matIndex = meshMats[meshIndex];
	bsdf->materialIndex = matIndex;

	// Interpolate face normal and UV coordinates
	const float b1 = rayHit->b1;
	const float b2 = rayHit->b2;
#if defined(PARAM_ACCEL_MQBVH)
	const float3 geometryN = Mesh_GetGeometryNormal(iVertices, iTriangles, triangleID);
	VSTORE3F(geometryN, &bsdf->hitPoint.geometryN.x);
	float3 shadeN = Mesh_InterpolateNormal(iVertNormals, iTriangles, triangleID, b1, b2);
	shadeN = Transform_InvApplyVector(&meshDesc->trans, shadeN);
	const float2 hitPointUV = Mesh_InterpolateUV(iVertUVs, iTriangles, triangleID, b1, b2);
#else
	const float3 geometryN = Mesh_GetGeometryNormal(vertices, triangles, currentTriangleIndex);
	VSTORE3F(geometryN, &bsdf->hitPoint.geometryN.x);
	float3 shadeN = Mesh_InterpolateNormal(vertNormals, triangles, currentTriangleIndex, b1, b2);
	const float2 hitPointUV = Mesh_InterpolateUV(vertUVs, triangles, currentTriangleIndex, b1, b2);
#endif
	VSTORE2F(hitPointUV, &bsdf->hitPoint.uv.u);

#if (PARAM_DL_LIGHT_COUNT > 0)
	// Check if it is a light source
	bsdf->triangleLightSourceIndex = meshLights[currentTriangleIndex];
#endif

#if defined(PARAM_HAS_BUMPMAPS) || defined(PARAM_HAS_NORMALMAPS)
	__global Material *mat = &mats[matIndex];

#if defined(PARAM_HAS_NORMALMAPS)
	// Check if I have to apply normal mapping
	const uint normalTexIndex = mat->normalTexIndex;
	if (normalTexIndex != NULL_INDEX) {
		// Apply normal mapping
		const float3 color = Texture_GetSpectrumValue(&texs[normalTexIndex], &bsdf->hitPoint
			TEXTURES_PARAM);
		const float3 xyz = 2.f * color - 1.f;

		float3 v1, v2;
		CoordinateSystem(shadeN, &v1, &v2);
		shadeN = normalize((float3)(
				v1.x * xyz.x + v2.x * xyz.y + shadeN.x * xyz.z,
				v1.y * xyz.x + v2.y * xyz.y + shadeN.y * xyz.z,
				v1.z * xyz.x + v2.z * xyz.y + shadeN.z * xyz.z));
	}
#endif

#if defined(PARAM_HAS_BUMPMAPS)
	// Check if I have to apply bump mapping
	const uint bumpTexIndex = mat->bumpTexIndex;
	if (bumpTexIndex != NULL_INDEX) {
		// Apply bump mapping
		__global Texture *tex = &texs[bumpTexIndex];
		const float2 dudv = Texture_GetDuDv(tex, &bsdf->hitPoint
			TEXTURES_PARAM);

		const float b0 = Texture_GetFloatValue(tex, &bsdf->hitPoint
			TEXTURES_PARAM);

		float dbdu;
		if (dudv.s0 > 0.f) {
			// This is a simple trick. The correct code would require true differential information.
			VSTORE3F((float3)(hitPointP.x + dudv.s0, hitPointP.y, hitPointP.z), &bsdf->hitPoint.p.x);
			VSTORE2F((float2)(hitPointUV.s0 + dudv.s0, hitPointUV.s1), &bsdf->hitPoint.uv.u);
			const float bu = Texture_GetFloatValue(tex, &bsdf->hitPoint
				TEXTURES_PARAM);

			dbdu = (bu - b0) / dudv.s0;
		} else
			dbdu = 0.f;

		float dbdv;
		if (dudv.s1 > 0.f) {
			// This is a simple trick. The correct code would require true differential information.
			VSTORE3F((float3)(hitPointP.x, hitPointP.y + dudv.s1, hitPointP.z), &bsdf->hitPoint.p.x);
			VSTORE2F((float2)(hitPointUV.s0, hitPointUV.s1 + dudv.s1), &bsdf->hitPoint.uv.u);
			const float bv = Texture_GetFloatValue(tex, &bsdf->hitPoint
				TEXTURES_PARAM);

			dbdv = (bv - b0) / dudv.s1;
		} else
			dbdv = 0.f;

		// Restore p and uv value
		VSTORE3F(hitPointP, &bsdf->hitPoint.p.x);
		VSTORE2F(hitPointUV, &bsdf->hitPoint.uv.u);

		const float3 bump = (float3)(dbdu, dbdv, 1.f);

		float3 v1, v2;
		CoordinateSystem(shadeN, &v1, &v2);
		shadeN = normalize((float3)(
				v1.x * bump.x + v2.x * bump.y + shadeN.x * bump.z,
				v1.y * bump.x + v2.y * bump.y + shadeN.y * bump.z,
				v1.z * bump.x + v2.z * bump.y + shadeN.z * bump.z));
	}
#endif
#endif

	Frame_SetFromZ(&bsdf->frame, shadeN);

	VSTORE3F(shadeN, &bsdf->hitPoint.shadeN.x);
}

float3 BSDF_Evaluate(__global BSDF *bsdf,
		const float3 generatedDir, BSDFEvent *event, float *directPdfW
		MATERIALS_PARAM_DECL) {
	//const Vector &eyeDir = fromLight ? generatedDir : hitPoint.fixedDir;
	//const Vector &lightDir = fromLight ? hitPoint.fixedDir : generatedDir;
	const float3 eyeDir = VLOAD3F(&bsdf->hitPoint.fixedDir.x);
	const float3 lightDir = generatedDir;
	const float3 geometryN = VLOAD3F(&bsdf->hitPoint.geometryN.x);

	const float dotLightDirNG = dot(lightDir, geometryN);
	const float absDotLightDirNG = fabs(dotLightDirNG);
	const float dotEyeDirNG = dot(eyeDir, geometryN);
	const float absDotEyeDirNG = fabs(dotEyeDirNG);

	if ((absDotLightDirNG < DEFAULT_COS_EPSILON_STATIC) ||
			(absDotEyeDirNG < DEFAULT_COS_EPSILON_STATIC))
		return BLACK;

	__global Material *mat = &mats[bsdf->materialIndex];
	const float sideTest = dotEyeDirNG * dotLightDirNG;
	const BSDFEvent matEvent = Material_GetEventTypes(mat
			MATERIALS_PARAM);
	if (((sideTest > 0.f) && !(matEvent & REFLECT)) ||
			((sideTest < 0.f) && !(matEvent & TRANSMIT)))
		return BLACK;

	__global Frame *frame = &bsdf->frame;
	const float3 localLightDir = Frame_ToLocal(frame, lightDir);
	const float3 localEyeDir = Frame_ToLocal(frame, eyeDir);
	const float3 result = Material_Evaluate(mat, &bsdf->hitPoint,
			localLightDir, localEyeDir,	event, directPdfW
			MATERIALS_PARAM);

	// Adjoint BSDF
//	if (fromLight) {
//		const float absDotLightDirNS = AbsDot(lightDir, shadeN);
//		const float absDotEyeDirNS = AbsDot(eyeDir, shadeN);
//		return result * ((absDotLightDirNS * absDotEyeDirNG) / (absDotEyeDirNS * absDotLightDirNG));
//	} else
		return result;
}

float3 BSDF_Sample(__global BSDF *bsdf, const float u0, const float u1,
		float3 *sampledDir, float *pdfW, float *cosSampledDir, BSDFEvent *event
		MATERIALS_PARAM_DECL) {
	const float3 fixedDir = VLOAD3F(&bsdf->hitPoint.fixedDir.x);
	const float3 localFixedDir = Frame_ToLocal(&bsdf->frame, fixedDir);
	float3 localSampledDir;

	const float3 result = Material_Sample(&mats[bsdf->materialIndex], &bsdf->hitPoint,
			localFixedDir, &localSampledDir, u0, u1,
#if defined(PARAM_HAS_PASSTHROUGH)
			bsdf->hitPoint.passThroughEvent,
#endif
			pdfW, cosSampledDir, event
			MATERIALS_PARAM);
	if (Spectrum_IsBlack(result))
		return 0.f;

	*sampledDir = Frame_ToWorld(&bsdf->frame, localSampledDir);

	// Adjoint BSDF
//	if (fromLight) {
//		const float absDotFixedDirNS = fabsf(localFixedDir.z);
//		const float absDotSampledDirNS = fabsf(localSampledDir.z);
//		const float absDotFixedDirNG = AbsDot(fixedDir, geometryN);
//		const float absDotSampledDirNG = AbsDot(*sampledDir, geometryN);
//		return result * ((absDotFixedDirNS * absDotSampledDirNG) / (absDotSampledDirNS * absDotFixedDirNG));
//	} else
		return result;
}

bool BSDF_IsDelta(__global BSDF *bsdf
		MATERIALS_PARAM_DECL) {
	return Material_IsDelta(&mats[bsdf->materialIndex]
			MATERIALS_PARAM);
}

#if (PARAM_DL_LIGHT_COUNT > 0)
float3 BSDF_GetEmittedRadiance(__global BSDF *bsdf,
		__global TriangleLight *triLightDefs, float *directPdfA
		MATERIALS_PARAM_DECL) {
	const uint triangleLightSourceIndex = bsdf->triangleLightSourceIndex;
	if (triangleLightSourceIndex == NULL_INDEX)
		return BLACK;
	else
		return TriangleLight_GetRadiance(&triLightDefs[triangleLightSourceIndex],
				&bsdf->hitPoint, directPdfA
				MATERIALS_PARAM);
}
#endif

#if defined(PARAM_HAS_PASSTHROUGH)
float3 BSDF_GetPassThroughTransparency(__global BSDF *bsdf
		MATERIALS_PARAM_DECL) {
	const float3 localFixedDir = Frame_ToLocal(&bsdf->frame, VLOAD3F(&bsdf->hitPoint.fixedDir.x));

	return Material_GetPassThroughTransparency(&mats[bsdf->materialIndex],
			&bsdf->hitPoint, localFixedDir, bsdf->hitPoint.passThroughEvent
			MATERIALS_PARAM);
}
#endif

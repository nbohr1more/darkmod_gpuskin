/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#version 330 core
#extension GL_ARB_texture_query_lod: enable
#extension GL_ARB_texture_gather: enable

#define STGATILOV_USEGATHER 1

out vec4 FragColor;

#pragma tdm_include "tdm_utils.glsl"
#pragma tdm_include "stages/interaction/interaction.common.fs.glsl"
#pragma tdm_include "tdm_shadowstencilsoft.glsl"
#pragma tdm_include "tdm_shadowstencilcontact.glsl"
#define TDM_allow_ARB_texture_gather STGATILOV_USEGATHER
#define SHADOWMAP_NO_FALL_ANGLE_LIMITER 1
#pragma tdm_include "tdm_shadowmaps.glsl"

uniform vec4 u_shadowRect;
uniform sampler2D u_shadowMap;
uniform int u_alphaSoftShadowsQuality;

in vec3 var_LightDirWorld;

uniform usampler2D u_stencilTexture;
uniform sampler2D u_depthTexture;

uniform sampler2D u_stencilMipmapsTexture;
uniform ivec2 u_stencilMipmapsLevel;
uniform vec4 u_stencilMipmapsScissor;

// screen-space contact march for stencil shadows (r_stencilSoftContact): darkens depth-based leaks in creases
// (wall meets ground, desk meets ground). It can only darken. Uses the same tuning cvars as shadow map contact shadows.
uniform vec3 u_globalViewOrigin;
uniform vec3 u_globalLightOrigin;
uniform vec3 u_shadowMapContact;	// (strength, ray length in world units, number of steps)
uniform vec2 u_shadowMapContactCrease;	// (1 = march only in creases measured from the depth buffer, 0 = everywhere; ring radius in world units)
uniform vec2 u_shadowMapContactBlur;
uniform float u_shadowMapContactThickness;
uniform vec2 u_shadowMapSsaoFade;	// full within x units from the viewer, fades out at y units (y = 0: no fading)
uniform int u_stencilContactDebug;	// r_stencilSoftContactDebug
// debug values of the last stencilContactLit call: (crease weight, weight after near-shadow gate, occlusion)
vec3 g_contactDbg = vec3(0.0);

float stencilContactLit(vec3 objectToLight, vec3 objectNormal, float softShadow) {
	float contactFade = 1.0;
	if (u_shadowMapSsaoFade.y > 0.0) {
		float viewDist = length(u_globalViewOrigin - u_globalLightOrigin + var_LightDirWorld);
		contactFade = 1.0 - smoothstep(u_shadowMapSsaoFade.x, u_shadowMapSsaoFade.y, viewDist);
		if (contactFade <= 0.01)
			return 1.0;
	}
	vec2 texSize = vec2(textureSize(u_stencilTexture, 0));
	vec2 pixSize = 1.0 / texSize;
	vec2 uvMin = 0.5 * pixSize;
	vec2 uvMax = 1.0 - 0.5 * pixSize;
	vec3 P_v = viewPosFromUvDepth(u_projectionMatrix, gl_FragCoord.xy * pixSize, gl_FragCoord.z);
	mat3 mv3 = mat3(u_modelViewMatrix);
	vec3 N_v = normalize(mv3 * objectNormal);
	if (dot(N_v, -P_v) < 0.0)
		N_v = -N_v;
	float noise = interleavedGradientNoise(gl_FragCoord.xy);
	if (u_shadowMapContactCrease.x > 0.5) {
		contactFade *= contactCreaseFactor(u_depthTexture, u_projectionMatrix, P_v, N_v, u_shadowMapContactCrease.y, noise, uvMin, uvMax);
		g_contactDbg.x = contactFade;
		if (contactFade <= 0.01)
			return 1.0;
	}
	if (softShadow > 0.98) {
		// a leak is a lit pixel right next to shadow: far from any stencil shadow the march only finds false blockers
		// taps stay inside the part of the stencil texture which was filled for this light
		vec2 tapMin = uvMin;
		vec2 tapMax = uvMax;
		if (u_stencilMipmapsLevel.x >= 0) {
			tapMin = max(tapMin, (u_stencilMipmapsScissor.xy + 0.5) * pixSize);
			tapMax = min(tapMax, (u_stencilMipmapsScissor.zw + 0.5) * pixSize);
		}
		float nearShadow = 0.0;
		for (int ring = 0; ring < 2 && nearShadow < 1.0; ring++) {
			float R = ring == 0 ? 4.0 : 10.0;
			for (int i = 0; i < 8; i++) {
				float a = (float(i) + 0.5 * float(ring)) * 0.785398;
				vec2 tc = clamp((gl_FragCoord.xy + vec2(cos(a), sin(a)) * R) * pixSize, tapMin, tapMax);
				nearShadow += fetchStencilShadowTexture(u_stencilTexture, tc) < 0.5 ? 0.5 : 0.0;
			}
		}
		contactFade *= smoothstep(0.0, 1.0, nearShadow);
		g_contactDbg.y = contactFade;
		if (contactFade <= 0.01)
			return 1.0;
	}
	vec3 lightPos_v = P_v + mv3 * objectToLight;
	float occlusion = ssabssContactOcclusionBlurred(
		u_depthTexture, u_projectionMatrix,
		P_v, N_v, lightPos_v, u_shadowMapContact.y, int(u_shadowMapContact.z + 0.5),
		noise, uvMin, uvMax, u_shadowMapContactThickness, u_shadowMapContactBlur.x, u_shadowMapContactBlur.y
	);
	g_contactDbg.z = occlusion;
	return 1.0 - u_shadowMapContact.x * contactFade * occlusion;
}

void main() {
	InteractionGeometry props;
	vec3 worldParallax;
	FragColor.rgb = computeInteraction(props, worldParallax);

	vec3 objectToLight = var_TangentBitangentNormalMatrix * var_LightDirLocal;
	vec3 objectNormal = var_TangentBitangentNormalMatrix[2];

	if (checkFlag(u_flags, SFL_INTERACTION_SHADOWS) && u_softShadowsQuality > 0) {
		float softShadow = computeStencilSoftShadow(
			u_stencilTexture, u_depthTexture,
			objectToLight, objectNormal,
			u_modelViewMatrix, u_projectionMatrix,
			u_softShadowsQuality, u_softShadowsRadius,
			u_stencilMipmapsTexture, u_stencilMipmapsLevel, u_stencilMipmapsScissor
		);
		if (checkFlag(u_flags, SFL_INTERACTION_SHADOW_MAP_CONTACT) && softShadow > 0.02)
			softShadow = min(softShadow, stencilContactLit(objectToLight, objectNormal, softShadow));
		FragColor.rgb *= softShadow;
		// r_stencilSoftContactDebug: 1 = crease weight, 2 = weight after the near-shadow gate, 3 = occlusion, 4 = shadow term (white = lit)
		if (u_stencilContactDebug == 1) FragColor.rgb = vec3(g_contactDbg.x);
		if (u_stencilContactDebug == 2) FragColor.rgb = vec3(g_contactDbg.y);
		if (u_stencilContactDebug == 3) FragColor.rgb = vec3(g_contactDbg.z);
		if (u_stencilContactDebug == 4) FragColor.rgb = vec3(softShadow);
	}
	// alpha-tested casters (foliage, grates) are not in stencil: they are in a small shadow map
	// (skipped if the pixel is already unlit: facing away, or in the stencil shadow)
	if (checkFlag(u_flags, SFL_INTERACTION_SHADOWS) && checkFlag(u_flags, SFL_INTERACTION_ALPHA_SHADOW) && any(greaterThan(FragColor.rgb, vec3(0.0)))) {
		vec3 worldNormal = mat3(u_modelMatrix) * var_TangentBitangentNormalMatrix[2];
		if (props.localN.z < 0.0)	// #5862: reverse-sided material with Z < 0 normalmap
			worldNormal = -worldNormal;
		// thin two-sided casters (grates) are lit from both sides: the normal must face the light,
		// otherwise the shadow map code treats the back side as unlit (fallAngleLimiter = 0)
		if (dot(worldNormal, var_LightDirWorld) < 0.0)
			worldNormal = -worldNormal;
		float alphaCoeff = computeShadowMapCoefficient(
			-var_LightDirWorld, worldNormal,
			u_shadowMap, u_shadowRect,
			u_alphaSoftShadowsQuality, u_softShadowsRadius,	// PCSS (contact hardening) of the shadow map code, independent of stencil softness
			false	// alpha page is rendered two-sided, no cull-front tricks
		);
		FragColor.rgb *= alphaCoeff;
	}
	FragColor.a = 1.0;
}

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
#extension GL_ARB_texture_gather: enable
#extension GL_ARB_texture_query_lod: enable

#define STGATILOV_USEGATHER 1

out vec4 FragColor;

#pragma tdm_include "stages/interaction/interaction.common.fs.glsl"
#define TDM_allow_ARB_texture_gather STGATILOV_USEGATHER
#pragma tdm_include "tdm_shadowmaps.glsl"
#pragma tdm_include "tdm_shadowstencilsoft_ssabss.glsl"

uniform vec4 u_shadowRect;
uniform sampler2D u_shadowMap;
uniform vec2 u_renderResolution;
uniform int u_shadowMapDebug;
uniform vec2 u_shadowMapSsaoFade;	// contact march is full within x units from the viewer, fades out to nothing at y units (y = 0: no fading)
uniform vec3 u_globalViewOrigin;
uniform vec3 u_globalLightOrigin;
uniform sampler2D u_depthTexture;	// scene depth, for the contact ray march
uniform vec2 u_shadowMapContactCrease;	// (1 = march only in creases measured from the depth buffer, 0 = everywhere; ring radius in world units)
uniform vec2 u_shadowMapContactBlur;	// (radius of the occlusion blur on the surface in world units, 0 = off; weight of the darkest sample vs the mean)
uniform float u_shadowMapContactThickness;	// assumed thickness of visible surfaces, world units
uniform vec3 u_shadowMapContact;	// (strength, ray length in world units, number of steps)

in vec3 var_LightDirWorld;

// hard shadow (1 = shadowed, 0 = lit) at the point of the surface which is o pixels away from the current one on screen
float leakShadowAt(vec3 dirToLight, vec3 dirDx, vec3 dirDy, vec2 o, vec3 worldNormal) {
	return 1.0 - computeShadowMapCoefficient(dirToLight + dirDx * o.x + dirDy * o.y, worldNormal, u_shadowMap, u_shadowRect, 0, u_softShadowsRadius, true);
}

void main() {
	InteractionGeometry props;
	vec3 worldParallax;
	FragColor.rgb = computeInteraction(props, worldParallax);

	vec3 worldNormal = mat3(u_modelMatrix) * var_TangentBitangentNormalMatrix[2];
	if (props.localN.z < 0.0)	// #5862: reverse-sided material with Z < 0 normalmap
		worldNormal = -worldNormal;

	if (checkFlag(u_flags, SFL_INTERACTION_SHADOWS)) {
		vec3 dirToLight = -var_LightDirWorld;
		if (checkFlag(u_flags, SFL_INTERACTION_PARALLAX_OFFSET_EXTERNAL_SHADOWS))
			dirToLight += worldParallax;
		// light vector changes linearly over the surface: derivatives give neighbor pixels on the same plane
		// (must be computed in uniform control flow)
		vec3 dirToLightDx = dFdx(dirToLight);
		vec3 dirToLightDy = dFdy(dirToLight);
		bool cullFrontMode = checkFlag(u_flags, SFL_INTERACTION_SHADOW_MAP_CULL_FRONT);
		float shadowsCoeff = computeShadowMapCoefficient(
			dirToLight, worldNormal,
			u_shadowMap, u_shadowRect,
			u_softShadowsQuality, u_softShadowsRadius,
			cullFrontMode
		);
		if (checkFlag(u_flags, SFL_INTERACTION_SHADOW_MAP_CONTACT) && shadowsCoeff > 0.02) {
			// screen-space ray march towards the light: a surface which is visible and lies between the pixel and the light shadows it,
			// no matter what the shadow map says (it leaks in creases and seams). This can only darken.
			float contactFade = 1.0;
			if (u_shadowMapSsaoFade.y > 0.0) {
				float viewDist = length(u_globalViewOrigin - u_globalLightOrigin + var_LightDirWorld);
				contactFade = 1.0 - smoothstep(u_shadowMapSsaoFade.x, u_shadowMapSsaoFade.y, viewDist);
			}
			vec2 pixSize = 1.0 / u_renderResolution;
			vec3 P_v = viewPosFromUvDepth(u_projectionMatrix, gl_FragCoord.xy * pixSize, gl_FragCoord.z);
			mat3 mv3 = mat3(u_modelViewMatrix);
			vec3 N_v = normalize(mv3 * var_TangentBitangentNormalMatrix[2]);
			if (dot(N_v, -P_v) < 0.0)
				N_v = -N_v;
			float contactNoise = interleavedGradientNoise(gl_FragCoord.xy);
			if (u_shadowMapContactCrease.x > 0.5 && contactFade > 0.01)
				contactFade *= contactCreaseFactor(u_depthTexture, u_projectionMatrix, P_v, N_v, u_shadowMapContactCrease.y, contactNoise, 0.5 * pixSize, 1.0 - 0.5 * pixSize);
			// a leak is a lit pixel right next to shadow: far from any shadow in the shadow map (e.g. around a candle
			// whose own geometry does not cast) the march only finds false blockers, so skip it there
			if (contactFade > 0.01 && shadowsCoeff > 0.98) {
				// soft count of shadowed taps (not a binary test): a pixel with two or more shadowed neighbors counts fully
				float nearShadow = 0.0;
				for (int ring = 0; ring < 2 && nearShadow < 1.0; ring++) {
					float R = ring == 0 ? 4.0 : 10.0;
					for (int i = 0; i < 8; i++) {
						float a = (float(i) + 0.5 * float(ring)) * 0.785398;
						nearShadow += leakShadowAt(dirToLight, dirToLightDx, dirToLightDy, vec2(cos(a), sin(a)) * R, worldNormal) > 0.5 ? 0.5 : 0.0;
					}
				}
				nearShadow = smoothstep(0.0, 1.0, nearShadow);
				contactFade *= nearShadow;
			}
			if (contactFade > 0.01) {
				vec3 objectToLight = var_TangentBitangentNormalMatrix * var_LightDirLocal;
				vec3 lightPos_v = P_v + mv3 * objectToLight;
				float occlusion = ssabssContactOcclusionBlurred(
					u_depthTexture, u_projectionMatrix,
					P_v, N_v, lightPos_v, u_shadowMapContact.y, int(u_shadowMapContact.z + 0.5),
					contactNoise,
					0.5 * pixSize, 1.0 - 0.5 * pixSize, u_shadowMapContactThickness, u_shadowMapContactBlur.x, u_shadowMapContactBlur.y
				);
				shadowsCoeff = min(shadowsCoeff, 1.0 - u_shadowMapContact.x * contactFade * occlusion);
			}
		}
		if (u_shadowMapDebug == 0) {
			FragColor.rgb *= shadowsCoeff;
		} else {
			// debug views (r_shadowMapDebug): the color of each light is added, so look at one light at a time
			bool cullFront = checkFlag(u_flags, SFL_INTERACTION_SHADOW_MAP_CULL_FRONT);
			vec3 dbg = vec3(shadowsCoeff);
			if (u_shadowMapDebug == 2) {
				// center sample decision: black = shadowed, white = lit, magenta = lit because shadow map texel is empty,
				// red = lit but within the error margin band (depth compare is ambiguous)
				float state = debugShadowMapCenter(dirToLight, worldNormal, u_shadowMap, u_shadowRect, cullFront).x;
				dbg = state < 0.5 ? vec3(0.0) : (state < 1.5 ? vec3(1.0) : (state < 2.5 ? vec3(1.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
			} else if (u_shadowMapDebug == 5) {
				// absolute error margin of depth compare in shadow map texels (white = 50 texels)
				dbg = vec3(debugShadowMapCenter(dirToLight, worldNormal, u_shadowMap, u_shadowRect, cullFront).y / 50.0);
			}
			FragColor.rgb = dbg;
		}
	}
	FragColor.a = 1.0;
}

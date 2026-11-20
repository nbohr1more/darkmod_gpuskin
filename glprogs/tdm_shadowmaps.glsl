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

#pragma tdm_include "tdm_utils.glsl"
#pragma tdm_include "tdm_poissondisk.glsl"  // note: adds uniform

// 0 = legacy: the same Poisson pattern for every pixel, uniform weights
// 1 = the pattern is rotated by a per-pixel angle (bands become fine noise) and samples are weighted by a tent (smoother penumbra edge)
uniform float u_softShadowsFilter;

mat2 softShadowsPatternRotation() {
	if (u_softShadowsFilter < 0.5)
		return mat2(1.0, 0.0, 0.0, 1.0);
	vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
	float a = 6.2831853 * fract(magic.z * fract(dot(gl_FragCoord.xy, magic.xy)));
	float c = cos(a), s = sin(a);
	return mat2(c, s, -s, c);
}
float softShadowsSampleWeight(vec2 s) {
	return u_softShadowsFilter < 0.5 ? 1.0 : 1.0 - 0.6 * dot(s, s);
}

vec3 CubeMapDirectionToUv(vec3 v, out int faceIdx) {
	vec3 v1 = abs(v);
	float maxV = max(v1.x, max(v1.y, v1.z));
	faceIdx = 0;
	if(maxV == v.x) {
		v1 = -v.zyx;
	}
	else if(maxV == -v.x) {
		v1 = v.zyx * vec3(1, -1, 1);
		faceIdx = 1;
	}
	else if(maxV == v.y) {
		v1 = v.xzy * vec3(1, 1, -1);
		faceIdx = 2;
	}
	else if(maxV == -v.y) {
		v1 = v.xzy * vec3(1, -1, 1);
		faceIdx = 3;
	}
	else if(maxV == v.z) {
		v1 = v.xyz * vec3(1, -1, -1);
		faceIdx = 4;
	}
	else { //if(maxV == -v.z) {
		v1 = v.xyz * vec3(-1, -1, 1);
		faceIdx = 5;
	}
	v1.xy /= -v1.z;
	return v1;
}

//shadowRect defines where one face of this light is within the map atlas:
//  (offsetX, offsetY, ?, size)     (all from 0..1)
//lightVec is world-space vector from the light origin to the fragment being considered
//returns positive distance along face normal of cube map (i.e. along coordinate where lightVec has maximum absolute value)
float ShadowAtlasForVector(in sampler2D shadowMapTexture, vec4 shadowRect, vec3 lightVec) {
	int faceIdx;
	vec3 v1 = CubeMapDirectionToUv(lightVec, faceIdx);
	vec2 texSize = textureSize(shadowMapTexture, 0);
	vec2 shadow2d = (v1.xy * .5 + vec2(.5) ) * shadowRect.ww + shadowRect.xy;
	shadow2d.x += (shadowRect.w + 1./texSize.x) * faceIdx;
	float d = textureLod(shadowMapTexture, shadow2d, 0).r;
	return 1 / (1 - d);
}
#if TDM_allow_ARB_texture_gather
vec4 ShadowAtlasForVector4(in sampler2D shadowMapTexture, vec4 shadowRect, vec3 lightVec, out vec4 sampleWeights) {
	int faceIdx;
	vec3 v1 = CubeMapDirectionToUv(lightVec, faceIdx);
	vec2 texSize = textureSize(shadowMapTexture, 0);
	vec2 shadow2d = (v1.xy * .5 + vec2(.5) ) * shadowRect.ww + shadowRect.xy;
	shadow2d.x += (shadowRect.w + 1./texSize.x) * faceIdx;
#if GL_ARB_texture_gather
	vec4 d = textureGather(shadowMapTexture, shadow2d);
#else
	vec4 d = textureLod(shadowMapTexture, shadow2d, 0).rrrr;
#endif
	vec2 wgt = fract(shadow2d * texSize - 0.5);
	vec2 mwgt = vec2(1) - wgt;
	sampleWeights = vec4(mwgt.x, wgt.x, wgt.x, mwgt.x) * vec4(wgt.y, wgt.y, mwgt.y, mwgt.y);
	return vec4(1) / (vec4(1) - d);
}
#endif

float computeShadowMapCoefficient(
	vec3 worldFromLight, vec3 worldNormal,
	sampler2D shadowMap, vec4 shadowRect,
	int softQuality, float softRadius, bool shadowMapCullFrontHack
) {
	float shadowMapResolution = (textureSize(shadowMap, 0).x * shadowRect.w);

	//get unit direction from light to current fragment
	vec3 L = normalize(worldFromLight);
	//find maximum absolute coordinate in light vector
	vec3 absL = abs(worldFromLight);
	float maxAbsL = max(absL.x, max(absL.y, absL.z));

	//note: using different normal affects near-tangent lighting greatly
	float lightFallAngle = -dot(worldNormal, L);

	//note: choosing normal and how to cap angles is the hardest question for now
	//this has large effect on near-tangent surfaces (mostly the curved ones)
#ifdef SHADOWMAP_NO_FALL_ANGLE_LIMITER
	//thin alpha-tested casters: the receiver can be the caster itself, angle must not zero the light
	float fallAngleLimiter = 1.0;
#else
	float fallAngleLimiter = smoothstep(0.01, 0.05, lightFallAngle);
#endif
	//some very generic error estimation...
	float errorMargin = 5.0 * maxAbsL / ( shadowMapResolution * max(lightFallAngle, 0.1) );
	if (shadowMapCullFrontHack)
	   errorMargin *= -.5;

	//process central shadow sample
	float centerFragZ = maxAbsL;
#if STGATILOV_USEGATHER && defined(GL_ARB_texture_gather)
	vec4 wgt;
	vec4 centerBlockerZ4 = ShadowAtlasForVector4(shadowMap, shadowRect, L, wgt);
	float lit = dot(wgt, step(centerFragZ - errorMargin, centerBlockerZ4));
	float centerBlockerZ = min(min(centerBlockerZ4.x, centerBlockerZ4.y), min(centerBlockerZ4.z, centerBlockerZ4.w));
#else
	float centerBlockerZ = ShadowAtlasForVector(shadowMap, shadowRect, L);
	float lit = float(centerBlockerZ >= centerFragZ - errorMargin);
#endif
	if (softQuality == 0)
		return lit * fallAngleLimiter;

	float lightDist = max(length(worldFromLight), 1e-4);
	//this is (1 / cos(phi)), where phi is angle between light direction and normal of the pierced cube face
	float secFallAngle = lightDist / maxAbsL;
	//find two unit directions orthogonal to light direction
	vec3 nonCollinear = vec3(1, 0, 0);
	if (absL.x == maxAbsL)
		nonCollinear = vec3(0, 1, 0);
	vec3 orthoAxisX = normalize(cross(L, nonCollinear));
	vec3 orthoAxisY = cross(L, orthoAxisX);

	//search for blockers in a cone with rather large angle
	float searchAngle = max(0.03 * softRadius, 1e-5);    //TODO: this option is probably very important
	const float minBlurInShadowPixels = 5.0;
	float minBlurRel = minBlurInShadowPixels / max(secFallAngle * shadowMapResolution, 1.0);

	mat2 patternRot = softShadowsPatternRotation();
	float sumW = 0.0;
	float sumWU = 0.0;
	float sumWUTight = 0.0;
	float sumWU3 = 0.0;
	float sumWU4 = 0.0;
	float sumWInv2 = 0.0;
	float sumWInv3 = 0.0;
	float uMax = 0.0;
	float uMin = 1.0;
	int blockerCnt = 0;

	if (centerBlockerZ < centerFragZ - errorMargin) {
		float b = min(softRadius * max(centerFragZ - centerBlockerZ, 0.0) / max(centerBlockerZ, 1e-4) / lightDist, searchAngle);
		float u = b / searchAngle;
		float u3 = u * u * u;
		float uSafe = max(u, 0.02);
		float invU = 1.0 / uSafe;
		float invU2 = invU * invU;
		sumW += 1.0;
		sumWU += u;
		sumWUTight += uSafe;
		sumWU3 += u3;
		sumWU4 += u3 * u;
		sumWInv2 += invU2;
		sumWInv3 += invU2 * invU;
		uMax = max(uMax, u);
		uMin = min(uMin, uSafe);
		blockerCnt++;
	}

	// Dedicated near-field ring around minBlurRel so close blockers never slip between outer rings
	if (softQuality >= 4) {
		float nearSearch = min(0.85 * minBlurRel, searchAngle);
		for (int k = 0; k < 4; k++) {
			float ang = 1.5707963 * float(k);
			vec2 sp = patternRot * vec2(cos(ang), sin(ang));
			vec3 perturbedLightDir = normalize(L + nearSearch * (sp.x * orthoAxisX + sp.y * orthoAxisY));
			float blockerZ = ShadowAtlasForVector(shadowMap, shadowRect, perturbedLightDir);
			float dotDpL = max(max(abs(perturbedLightDir.x), abs(perturbedLightDir.y)), abs(perturbedLightDir.z));
			float distCoeff = lightFallAngle / max(-dot(worldNormal, perturbedLightDir), 1e-3) * (dotDpL * secFallAngle);
			float fragZ = centerFragZ * distCoeff;
			if (blockerZ < fragZ - errorMargin) {
				float b = min(softRadius * max(fragZ - blockerZ, 0.0) / max(blockerZ, 1e-4) / lightDist, searchAngle);
				float u = b / searchAngle;
				float u3 = u * u * u;
				float uSafe = max(u, 0.02);
				float invU = 1.0 / uSafe;
				float invU2 = invU * invU;
				sumW += 1.0;
				sumWU += u;
				sumWUTight += uSafe;
				sumWU3 += u3;
				sumWU4 += u3 * u;
				sumWInv2 += invU2;
				sumWInv3 += invU2 * invU;
				uMax = max(uMax, u);
				uMin = min(uMin, uSafe);
				blockerCnt++;
			}
		}
	}

	for (int i = 0; i < softQuality; i++) {
		vec2 s = u_softShadowsSamples[i];
		// Quadratically concentrate blocker search samples toward the center while still reaching searchAngle at rim
		vec2 sp = (patternRot * s) * length(s);
		float sampleWeight = softShadowsSampleWeight(s);
		vec3 perturbedLightDir = normalize(L + searchAngle * (sp.x * orthoAxisX + sp.y * orthoAxisY));
		float blockerZ = ShadowAtlasForVector(shadowMap, shadowRect, perturbedLightDir);
		float dotDpL = max(max(abs(perturbedLightDir.x), abs(perturbedLightDir.y)), abs(perturbedLightDir.z));
		float distCoeff = lightFallAngle / max(-dot(worldNormal, perturbedLightDir), 1e-3) * (dotDpL * secFallAngle);
		float fragZ = centerFragZ * distCoeff;
		//note: only things which may potentially occlude are accumulated
		if (blockerZ < fragZ - errorMargin) {
			float b = min(softRadius * max(fragZ - blockerZ, 0.0) / max(blockerZ, 1e-4) / lightDist, searchAngle);
			float u = b / searchAngle;
			float u3 = u * u * u;
			float uSafe = max(u, 0.02);
			float invU = 1.0 / uSafe;
			float invU2 = invU * invU;
			sumW += sampleWeight;
			sumWU += sampleWeight * u;
			sumWUTight += sampleWeight * uSafe;
			sumWU3 += sampleWeight * u3;
			sumWU4 += sampleWeight * u3 * u;
			sumWInv2 += sampleWeight * invU2;
			sumWInv3 += sampleWeight * invU2 * invU;
			uMax = max(uMax, u);
			uMin = min(uMin, uSafe);
			blockerCnt++;
		}
	}
	//shortcut: no blockers in search angle => fully lit
	if (blockerCnt == 0)
		return fallAngleLimiter;

	float m3 = 0.02 * uMax * uMax * uMax;
	float uWide = (m3 * sumWU + sumWU4) / max(m3 * sumW + sumWU3, 1e-12);
	float n3 = uMin * uMin * uMin;
	float uTight = (0.02 * sumWUTight + n3 * sumWInv2) / max(0.02 * sumW + n3 * sumWInv3, 1e-12);
	float blurWide = max(uWide * searchAngle, minBlurRel);
	float blurTight = max(uTight * searchAngle, minBlurRel);

	// Wide PCF pass: evaluates all blockers at blurWide.
	// When a tap hits a near blocker whose penumbra cannot reach offset r,
	// re-sample along the same light-disk ray direction at that blocker's own penumbra scale
	// so open-air near blockers do not cast dark halos while solid sloped casters remain opaque.
	float centerWeightWide = (softQuality > 4) ? 0.0 : 0.25;
	float litWide = lit * centerWeightWide;
	float weightSumWide = centerWeightWide;
	for (int i = 0; i < softQuality; i++) {
		vec2 s = u_softShadowsSamples[i];
		vec2 sp = patternRot * s;
		float sampleWeight = softShadowsSampleWeight(s);
		float r = length(s) * blurWide;
		vec3 perturbedLightDir = normalize(L + blurWide * (sp.x * orthoAxisX + sp.y * orthoAxisY));
		float dotDpL = max(max(abs(perturbedLightDir.x), abs(perturbedLightDir.y)), abs(perturbedLightDir.z));
		float distCoeff = lightFallAngle / max(-dot(worldNormal, perturbedLightDir), 1e-3) * (dotDpL * secFallAngle);
		float fragZ = centerFragZ * distCoeff;
		float blockerZ = ShadowAtlasForVector(shadowMap, shadowRect, perturbedLightDir);
		if (blockerZ >= fragZ - errorMargin) {
			litWide += sampleWeight;
		} else {
			float bThis = clamp(softRadius * max(fragZ - blockerZ, 0.0) / max(blockerZ, 1e-4) / lightDist, minBlurRel, searchAngle);
			if (r > 1e-6 && bThis < 0.8 * r) {
				vec3 pDir2 = normalize(L + bThis * (sp.x * orthoAxisX + sp.y * orthoAxisY));
				float dotDpL2 = max(max(abs(pDir2.x), abs(pDir2.y)), abs(pDir2.z));
				float distCoeff2 = lightFallAngle / max(-dot(worldNormal, pDir2), 1e-3) * (dotDpL2 * secFallAngle);
				float fragZ2 = centerFragZ * distCoeff2;
				float blockerZ2 = ShadowAtlasForVector(shadowMap, shadowRect, pDir2);
				if (blockerZ2 >= fragZ2 - errorMargin) {
					float vis = smoothstep(0.5 * r, 0.8 * r, bThis);
					litWide += sampleWeight * (1.0 - vis);
				}
			}
		}
		weightSumWide += sampleWeight;
	}
	litWide /= weightSumWide;

	// Conditional near pass: when a near blocker has a much tighter penumbra than blurWide,
	// evaluate at blurTight and smoothly ignore wide blockers so near occluders stay crisp
	// across wide penumbra boundaries without slicing single casters.
	float sep = 1.0 - smoothstep(0.35 * blurWide, 0.75 * blurWide, blurTight);
	if (sep <= 0.001)
		return litWide * fallAngleLimiter;

	float centerWeightNear = 0.25;
	float litNear = 0.0;
	if (centerBlockerZ >= centerFragZ - errorMargin) {
		litNear = centerWeightNear;
	} else {
		float bC = clamp(softRadius * max(centerFragZ - centerBlockerZ, 0.0) / max(centerBlockerZ, 1e-4) / lightDist, minBlurRel, searchAngle);
		float nwC = 1.0 - smoothstep(0.35 * blurWide, 0.75 * blurWide, bC);
		litNear = centerWeightNear * (1.0 - nwC);
	}
	float weightSumNear = centerWeightNear;
	for (int i = 0; i < softQuality; i++) {
		vec2 s = u_softShadowsSamples[i];
		vec2 sp = patternRot * s;
		float sampleWeight = softShadowsSampleWeight(s);
		vec3 perturbedLightDir = normalize(L + blurTight * (sp.x * orthoAxisX + sp.y * orthoAxisY));
		float dotDpL = max(max(abs(perturbedLightDir.x), abs(perturbedLightDir.y)), abs(perturbedLightDir.z));
		float distCoeff = lightFallAngle / max(-dot(worldNormal, perturbedLightDir), 1e-3) * (dotDpL * secFallAngle);
		float fragZ = centerFragZ * distCoeff;
		float blockerZ = ShadowAtlasForVector(shadowMap, shadowRect, perturbedLightDir);
		if (blockerZ >= fragZ - errorMargin) {
			litNear += sampleWeight;
		} else {
			float bThis = clamp(softRadius * max(fragZ - blockerZ, 0.0) / max(blockerZ, 1e-4) / lightDist, minBlurRel, searchAngle);
			float nearWeight = 1.0 - smoothstep(0.35 * blurWide, 0.75 * blurWide, bThis);
			litNear += sampleWeight * (1.0 - nearWeight);
		}
		weightSumNear += sampleWeight;
	}
	litNear /= weightSumNear;

	return min(litWide, litNear) * fallAngleLimiter;
}


// Debug helper: describes the decision of the central shadow map sample (no soft shadows).
// x: 0 = shadowed, 1 = lit, 2 = lit because the shadow map texel is empty (nothing was rendered there), 3 = lit, but within the error margin band (ambiguous)
// y: absolute value of error margin, in shadow map texels
vec2 debugShadowMapCenter(
	vec3 worldFromLight, vec3 worldNormal,
	sampler2D shadowMap, vec4 shadowRect, bool shadowMapCullFrontHack
) {
	float shadowMapResolution = (textureSize(shadowMap, 0).x * shadowRect.w);
	vec3 L = normalize(worldFromLight);
	vec3 absL = abs(worldFromLight);
	float maxAbsL = max(absL.x, max(absL.y, absL.z));
	float lightFallAngle = -dot(worldNormal, L);
	float errorMargin = 5.0 * maxAbsL / ( shadowMapResolution * max(lightFallAngle, 0.1) );
	if (shadowMapCullFrontHack)
		errorMargin *= -.5;
	float fragZ = maxAbsL;
	float blockerZ = ShadowAtlasForVector(shadowMap, shadowRect, L);
	float marginTexels = abs(errorMargin) * shadowMapResolution / maxAbsL;
	float state;
	if (blockerZ < fragZ - errorMargin)
		state = 0.0;
	else if (blockerZ > 1e6)
		state = 2.0;
	else if (abs(blockerZ - (fragZ - errorMargin)) < 0.5 * abs(errorMargin))
		state = 3.0;
	else
		state = 1.0;
	return vec2(state, marginTexels);
}
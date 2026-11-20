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
#pragma tdm_include "tdm_shadowstencilsoft_shared.glsl"
#pragma tdm_include "tdm_poissondisk.glsl"  // note: adds uniform (only used if SSABSS_VOGEL is 0)
#pragma tdm_include "tdm_shadowstencilsoft_ssabss.glsl"  // blocker search, also used by the penumbra pre-pass

// penumbra pre-pass buffer (r_softShadowsPenumbraBuffer): x = penumbra size factor for the farthest blocker, y = for the nearest one
uniform sampler2D u_penumbraTexture;
uniform int u_penumbraMode;	// 0 = no buffer, search blockers here; 1 = read the buffer

// Set to 0 if you prefer static sample pattern over per-pixel IGN rotation
#define SSABSS_ROTATE_SAMPLES 1
// 1 = golden-angle (Vogel) disk generated in the shader: any prefix of the samples is evenly spread,
// which allows to use fewer samples for small footprints (SSABSS_ADAPTIVE_SAMPLES)
// 0 = precomputed Poisson disk from u_softShadowsSamples (always uses all samples)
#define SSABSS_VOGEL 1
#define SSABSS_ADAPTIVE_SAMPLES SSABSS_VOGEL
#define SSABSS_MIN_SAMPLES 8
// Penumbra model, tuned to resemble the shadow map PCSS: blur = lightRadius * blockerToReceiver / lightToBlocker
// (the shadow map version limits it to 0.03 * lightRadius * lightDist, no saturation with distance)
// far field: penumbra of a blocker which is far from the receiver. Grows with the light distance, saturating slowly
#define SSABSS_FAR_SCALE 4.0
#define SSABSS_FAR_REF_DIST 90.0
// several overlapping casters: the shadow is computed with two penumbra widths, from the nearest blocker found (narrow)
// and from the farthest blocker found (wide), and the darker of the two wins. This way a close caster (apple) neither
// hardens nor brightens the penumbra of a farther one (wall). Needs SSABSS_VOGEL (narrow = the inner samples of the same disk).
// 1 = enabled, 0 = one penumbra width from the nearest blocker
#define SSABSS_TWO_PENUMBRAS 1
#define SSABSS_MIN_NARROW_SAMPLES 3
// brightness limits relative to the hard (unblurred) stencil at this pixel, as fraction of full light:
// a pixel which is inside some caster's shadow never gets brighter than SSABSS_SHADOWED_MAX_LIT (1.0 = no limit),
// a pixel which is lit by the stencil never gets darker than SSABSS_LIT_MIN_LIT (0.0 = no limit)
#define SSABSS_SHADOWED_MAX_LIT 0.6
#define SSABSS_LIT_MIN_LIT 0.0
// the tolerance of depth difference between receiver plane and sample grows with the blur radius (world units per world unit)
#define SSABSS_DEPTH_TOL_PER_RADIUS 0.03
// penumbra buffer: a blur tap contributes only if the penumbra of the tap pixel itself is wide enough to reach the center pixel
// (scatter-as-gather). This keeps narrow penumbras (close casters) and wide ones (far casters) from affecting each other.
// sharpness: how fast the weight changes with the difference of penumbra size factors (1 / width of the transition)
#define SSABSS_TAP_REACH 1
#define SSABSS_TAP_REACH_SHARPNESS 4.0

float fetchStencilShadowTexture(usampler2D stencilTexture, vec2 texCoord) {
	float stTex = float(texture(stencilTexture, texCoord).r);
	return clamp(129.0 - stTex, 0.0, 1.0);
}

float computeStencilSoftShadow(
	usampler2D stencilTexture, sampler2D depthTexture,
	vec3 objectToLight, vec3 objectNormal,
	mat4 modelViewMatrix, mat4 projectionMatrix,
	int softQuality, float softRadius,
	sampler2D stencilMipmapsTexture, ivec2 stencilMipmapsLevel, vec4 stencilMipmapsScissor
) {
	vec2 texSize = vec2(textureSize(stencilTexture, 0));
	vec2 pixSize = vec2(1.0, 1.0) / texSize;
	vec2 baseTexCoord = gl_FragCoord.xy * pixSize;

	// UV clamp bounds (keeps taps inside light scissor when mipmaps/scissor are active)
	vec2 uvMin = 0.5 * pixSize;
	vec2 uvMax = 1.0 - 0.5 * pixSize;
	if (stencilMipmapsLevel.x >= 0) {
		uvMin = max(uvMin, (stencilMipmapsScissor.xy + 0.5) * pixSize);
		uvMax = min(uvMax, (stencilMipmapsScissor.zw - 0.5) * pixSize);

		vec2 pixelCoords = clamp(gl_FragCoord.xy, stencilMipmapsScissor.xy, stencilMipmapsScissor.zw);
		vec2 mipmapsFullSize = textureSize(stencilMipmapsTexture, 0) * (1 << stencilMipmapsLevel.y);
		float mipmapValue = textureLod(
			stencilMipmapsTexture,
			pixelCoords / mipmapsFullSize,
			stencilMipmapsLevel.x - stencilMipmapsLevel.y
		).r;
		if (mipmapValue == 0.0)
			return 0.0;
		else if (mipmapValue == 1.0)
			return 1.0;
	}

	float stencil = fetchStencilShadowTexture(stencilTexture, baseTexCoord);
	float centerStencil = stencil;
	float sumWeight = 1.0;
	if (softQuality == 0) {
		return stencil;
	}

	float ign = interleavedGradientNoise(gl_FragCoord.xy);
	// decorrelated from the one used for rotation of the samples
	float ign2 = interleavedGradientNoise(gl_FragCoord.xy + vec2(5.588238, 5.588238));
	float rawLightDist = max(length(objectToLight), 1e-3);
	float lightRadius = softRadius;

	// 1. Bounded distance-based far penumbra (replaces unbounded lightDist / 66.6666)
	float r_far = lightRadius * SSABSS_FAR_SCALE * (rawLightDist / (rawLightDist + SSABSS_FAR_REF_DIST));

	// 2. View-space reconstruction & screen-space contact hardening
	vec3 P_v = viewPosFromUvDepth(projectionMatrix, baseTexCoord, gl_FragCoord.z);
	mat3 mv3 = mat3(modelViewMatrix);
	// interpolated vertex normal is not unit length
	vec3 N_v = normalize(mv3 * objectNormal);
	vec3 N_o = normalize(objectNormal);
	if (dot(N_v, -P_v) < 0.0) {
		// the surface is seen from behind: flip the normal everywhere in the same way
		N_v = -N_v;
		N_o = -N_o;
	}
	vec3 L_dir = normalize(mv3 * objectToLight);

	// penumbra size factors: from the pre-pass buffer, or search blockers right here
	vec2 kk;
	if (u_penumbraMode != 0) {
		kk = texelFetch(u_penumbraTexture, ivec2(gl_FragCoord.xy), 0).rg;
	} else {
		kk = ssabssContactFactors(
			depthTexture, projectionMatrix,
			P_v, N_v, L_dir, rawLightDist,
			lightRadius, softQuality, ign2,
			uvMin, uvMax
		);
	}
	float kFar = kk.x;
	float kNear = kk.y;
#if SSABSS_TWO_PENUMBRAS && SSABSS_VOGEL
	float k_contact = max(kFar, kNear);
	// the narrow penumbra is the same disk with smaller radius
	float narrowRatio = clamp(kNear / max(k_contact, 1e-4), 0.0, 1.0);
#else
	float k_contact = kNear;
#endif
	float blurRadiusWorld = r_far * k_contact;

	// 3. SSABSS tangent-plane basis + light-grazing elongation in object space
	vec3 objL = objectToLight / rawLightDist;
	vec3 perpL = objectToLight - dot(objectToLight, N_o) * N_o;
	vec3 alongDirW = (dot(perpL, perpL) > 1e-6)
		? normalize(perpL)
		: normalize(cross(N_o, abs(N_o.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
	vec3 orthoDirW = cross(N_o, alongDirW);

	float NdotL = dot(objL, N_o);
	alongDirW *= blurRadiusWorld / max(NdotL, 0.25);
	orthoDirW *= blurRadiusWorld;

	// Convert both vectors into screen UV space via exact clip-space derivative
	mat4 modelViewProjectionMatrix = projectionMatrix * modelViewMatrix;
	vec2 alongDir = (mat3(modelViewProjectionMatrix) * alongDirW).xy;
	vec2 orthoDir = (mat3(modelViewProjectionMatrix) * orthoDirW).xy;
	vec3 mvpRow3 = vec3(modelViewProjectionMatrix[0][3], modelViewProjectionMatrix[1][3], modelViewProjectionMatrix[2][3]);
	float along_w = dot(mvpRow3, alongDirW);
	float ortho_w = dot(mvpRow3, orthoDirW);

	vec2 thisNdc = 2.0 * baseTexCoord - vec2(1.0);
	alongDir -= thisNdc * along_w;
	orthoDir -= thisNdc * ortho_w;
	alongDir *= gl_FragCoord.w * 0.5;
	orthoDir *= gl_FragCoord.w * 0.5;

	// 4. Enforce mipmap upper bound on each axis separately (the elongated axis must not shrink the other one)
	// and a minimum 0.75px AA floor on each axis
	float lenX = length(alongDir * texSize);
	float lenY = length(orthoDir * texSize);
	float maxBlurAxisLength = computeMaxBlurAxisLength(texSize.y, softQuality);

	float scaleAlong = min(1.0, maxBlurAxisLength / max(lenX, 1e-5));
	float scaleOrtho = min(1.0, maxBlurAxisLength / max(lenY, 1e-5));
	alongDir *= scaleAlong;
	alongDirW *= scaleAlong;
	lenX *= scaleAlong;
	orthoDir *= scaleOrtho;
	orthoDirW *= scaleOrtho;
	lenY *= scaleOrtho;

	const float MIN_AXIS_PX = 0.75;
	if (lenX > 1e-5 && lenX < MIN_AXIS_PX) {
		alongDir *= MIN_AXIS_PX / lenX;
		lenX = MIN_AXIS_PX;
	} else if (lenX <= 1e-5) {
		alongDir = vec2(MIN_AXIS_PX, 0.0) * pixSize;
		lenX = MIN_AXIS_PX;
	}
	if (lenY > 1e-5 && lenY < MIN_AXIS_PX) {
		orthoDir *= MIN_AXIS_PX / lenY;
		lenY = MIN_AXIS_PX;
	} else if (lenY <= 1e-5) {
		orthoDir = vec2(0.0, MIN_AXIS_PX) * pixSize;
		lenY = MIN_AXIS_PX;
	}

	// View-space Z deltas of the tangent vectors (for silhouette-safe analytic plane depth check)
	float alongViewZ = (mv3 * alongDirW).z;
	float orthoViewZ = (mv3 * orthoDirW).z;

	float Z00 = -P_v.z;
	vec2 tanFovHalf = vec2(1.0 / projectionMatrix[0][0], 1.0 / projectionMatrix[1][1]);
	vec2 canonDerivs = 2.0 * Z00 * tanFovHalf;
	float depthTolFloor = 0.5 + SSABSS_DEPTH_TOL_PER_RADIUS * blurRadiusWorld;

	// 5. Number of samples: the footprint is an ellipse, sample density is one per avgSampleDist^2 pixels
	int numSamples = softQuality;
#if SSABSS_ADAPTIVE_SAMPLES
	float avgSampleDist = computeMaxBlurAxisLength(texSize.y, 1.0);
	int neededSamples = int(ceil(lenX * lenY / (avgSampleDist * avgSampleDist)));
	numSamples = min(softQuality, max(SSABSS_MIN_SAMPLES, neededSamples));
#endif

	float rotAngle = 0.0;
#if SSABSS_ROTATE_SAMPLES
	rotAngle = ign * 6.2831853;
#endif
#if !SSABSS_VOGEL
	float cRot = cos(rotAngle);
	float sRot = sin(rotAngle);
#endif

#if SSABSS_TWO_PENUMBRAS && SSABSS_VOGEL
	// Vogel disk is ordered by radius: the first samples cover the inner disk. Area ratio = narrowRatio^2
	int numNarrow = numSamples;
	if (narrowRatio < 0.999)
		numNarrow = clamp(int(ceil(float(numSamples) * narrowRatio * narrowRatio)), min(SSABSS_MIN_NARROW_SAMPLES, numSamples), numSamples);
	float stencilNarrow = stencil;
	float sumWeightNarrow = 1.0;
#endif

	for (int i = 0; i < numSamples; i++) {
#if SSABSS_VOGEL
		float sampleRadius = sqrt((float(i) + 0.5) / float(numSamples));
		float sampleAngle = float(i) * 2.39996323 + rotAngle;
		vec2 s = sampleRadius * vec2(cos(sampleAngle), sin(sampleAngle));
#else
		vec2 s = u_softShadowsSamples[i];
		s = vec2(cRot * s.x - sRot * s.y, sRot * s.x + cRot * s.y);
#endif
		vec2 delta = s.x * alongDir + s.y * orthoDir;
		vec2 sampleTexCoord = clamp(baseTexCoord + delta, uvMin, uvMax);

		// Analytic tangent-plane depth change (avoids dFdx/dFdy spikes at silhouette edges)
		float Zdiff = depthToZ(projectionMatrix, textureLod(depthTexture, sampleTexCoord, 0.0).r) - Z00;
		float tangentZdiff = -(s.x * alongViewZ + s.y * orthoViewZ);
		float deg45diff = dot(canonDerivs, abs(delta));
		float ZdiffTol = max(abs(tangentZdiff) * 0.4 + deg45diff * 0.18, depthTolFloor);

		float err = max(abs(Zdiff - tangentZdiff) - ZdiffTol * 0.5, 0.0);
		float q = err / ZdiffTol;
		float weight = exp(-q * q);
#if SSABSS_TAP_REACH
		if (u_penumbraMode != 0) {
			// penumbra of the tap pixel itself: does it reach the center pixel?
			float tapK = texelFetch(u_penumbraTexture, ivec2(sampleTexCoord * texSize), 0).r;
			float reach = length(s) * k_contact;
			weight *= clamp((tapK - reach) * SSABSS_TAP_REACH_SHARPNESS + 1.0, 0.0, 1.0);
		}
#endif

		float st = fetchStencilShadowTexture(stencilTexture, sampleTexCoord) * weight;
		stencil += st;
		sumWeight += weight;
#if SSABSS_TWO_PENUMBRAS && SSABSS_VOGEL
		if (i < numNarrow) {
			stencilNarrow += st;
			sumWeightNarrow += weight;
		}
#endif
	}

	float result = stencil / sumWeight;
#if SSABSS_TWO_PENUMBRAS && SSABSS_VOGEL
	// darker of the two penumbras
	result = min(result, stencilNarrow / sumWeightNarrow);
#endif
	// the stencil is a union of all casters, so this limits light leaking into any caster's shadow from blurring
	if (centerStencil < 0.5)
		result = min(result, SSABSS_SHADOWED_MAX_LIT);
	else
		result = max(result, SSABSS_LIT_MIN_LIT);
	return result;
}

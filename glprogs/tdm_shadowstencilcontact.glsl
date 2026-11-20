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

// Screen-space contact shadow functions for the stencil interaction shader (r_stencilSoftContact).
// Same functions as in tdm_shadowstencilsoft_ssabss.glsl (used by the shadow map shader), copied here
// because the stencil shader does not include that file. Needs viewPosFromUvDepth and depthToZ.
// Do not include both this file and tdm_shadowstencilsoft_ssabss.glsl in one shader.

// Screen-space contact shadow: ray march from the receiver towards the light through the depth buffer.
// Returns occlusion (0..1): 1 if a visible surface is found between the receiver and the light, faded out with the distance along the ray.
// It can only find what is visible on screen, so it is used only to add shadow where a shadow map has leaked.
// How much the pixel sits in a crease / concave corner, measured from the depth buffer alone (independent of SSAO quality):
// 8 points on a ring of the given world radius around the pixel, a crease has visible surface in front of its tangent plane.
float contactCreaseFactor(
	sampler2D depthTexture, mat4 projectionMatrix,
	vec3 P_v, vec3 N_v, float radius, float noise,
	vec2 uvMin, vec2 uvMax
) {
	float sum = 0.0;
	for (int i = 0; i < 8; i++) {
		float a = float(i) * 0.785398 + 0.3;	// fixed angles: a per-pixel rotation would turn the result into stipple
		vec3 Q_v = P_v + vec3(cos(a), sin(a), 0.0) * radius;
		vec4 clip = projectionMatrix * vec4(Q_v, 1.0);
		if (clip.w <= 0.0)
			continue;
		vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
		if (any(lessThan(uv, uvMin)) || any(greaterThan(uv, uvMax)))
			continue;
		float d = textureLod(depthTexture, uv, 0.0).r;
		vec3 diff = viewPosFromUvDepth(projectionMatrix, uv, d) - P_v;
		float len = max(length(diff), 1e-3);
		// the surface point is in front of the tangent plane, and close enough to be a part of the same corner
		sum += smoothstep(0.15, 0.45, dot(N_v, diff) / len) * (1.0 - smoothstep(2.0 * radius, 4.0 * radius, len));
	}
	return smoothstep(0.06, 0.25, sum / 8.0);
}

float ssabssContactOcclusion(
	sampler2D depthTexture, mat4 projectionMatrix,
	vec3 P_v, vec3 N_v, vec3 lightPos_v, float maxDist, int steps, float noise,
	vec2 uvMin, vec2 uvMax, float thicknessBase
) {
	float lightDist = max(length(lightPos_v - P_v), 1e-3);
	vec3 L_dir = (lightPos_v - P_v) / lightDist;
	// the ray never reaches the light: beyond it there are things which cannot shadow the pixel (a light is often inside of a small object)
	maxDist = min(maxDist, 0.8 * lightDist);
	// Every decision is a smooth function (no hit / miss tests): a binary test per sample with a different noise value for
	// every pixel gives stipple and bands, a smooth one only moves the edge of the occlusion a little
	float transmittance = 1.0;
	for (int i = 0; i < steps; i++) {
		// samples are in the middle of their intervals, the noise moves them by half an interval at most
		float frac = (float(i) + 0.5 + (noise - 0.5) * 0.25) / float(steps);
		float t = maxDist * frac * sqrt(frac);	// denser near the receiver
		vec3 Q_v = P_v + t * L_dir;

		vec4 clip = projectionMatrix * vec4(Q_v, 1.0);
		if (clip.w <= 0.0)
			continue;
		vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
		if (any(lessThan(uv, uvMin)) || any(greaterThan(uv, uvMax)))
			continue;

		float sceneDepth = textureLod(depthTexture, uv, 0.0).r;
		float dz = (-Q_v.z) - depthToZ(projectionMatrix, sceneDepth);	// > 0: the ray is behind the visible surface
		float bias = 0.012 * (-P_v.z) + 0.04 * t;
		// soft depth comparison: the occlusion grows over this range instead of jumping, it is wider farther from the receiver
		float softRange = 1.0 + 0.1 * t;
		// the occluder is thin in the ray direction: a surface far in front of the ray is not it
		// assumed thickness of every visible surface: a ray further behind it passes (it is not blocked by a small object in front of a far background).
		// grows with the distance from the camera, since depth gets coarser there
		float thickness = thicknessBase * (1.0 + (-Q_v.z) * 0.005) + 0.15 * t;
		float wDepth = smoothstep(bias, bias + softRange, dz) * (1.0 - smoothstep(bias + thickness, bias + thickness + 2.0 * softRange, dz));
		if (wDepth > 0.001) {
			vec3 diff = viewPosFromUvDepth(projectionMatrix, uv, sceneDepth) - P_v;
			float dist3D = max(length(diff), 1e-3);
			// the same conditions as for the blocker search, but smooth: above the tangent plane of the receiver, on the light side,
			// and the surface is not close to the light itself (candle holder, lamp body, or whatever the light is attached to)
			float wGeom = smoothstep(0.04, 0.12, dot(N_v, diff) / dist3D)
				* smoothstep(0.35, 0.6, dot(L_dir, diff) / dist3D)
				* smoothstep(0.2 * lightDist, 0.3 * lightDist, length(P_v + diff - lightPos_v));
			// the farther along the ray, the weaker
			float w = wDepth * wGeom * (1.0 - smoothstep(0.5, 1.0, frac));
			transmittance *= 1.0 - w;
		}
	}
	return 1.0 - transmittance;
}

// The same, averaged over the receiver position moved by blurRadius (world units) along the tangent plane: a blur of the occlusion
// on the surface, which softens the edges of the contact halo. Costs 5 marches, each with about half of the steps.
float ssabssContactOcclusionBlurred(
	sampler2D depthTexture, mat4 projectionMatrix,
	vec3 P_v, vec3 N_v, vec3 lightPos_v, float maxDist, int steps, float noise,
	vec2 uvMin, vec2 uvMax, float thicknessBase, float blurRadius, float darkBias
) {
	if (blurRadius <= 0.0)
		return ssabssContactOcclusion(depthTexture, projectionMatrix, P_v, N_v, lightPos_v, maxDist, steps, noise, uvMin, uvMax, thicknessBase);
	vec3 T = normalize(abs(N_v.y) < 0.9 ? cross(N_v, vec3(0.0, 1.0, 0.0)) : cross(N_v, vec3(1.0, 0.0, 0.0)));
	vec3 B = cross(N_v, T);
	int s = max(4, steps / 2);
	float center = ssabssContactOcclusion(depthTexture, projectionMatrix, P_v, N_v, lightPos_v, maxDist, s, noise, uvMin, uvMax, thicknessBase);
	float sum = 0.4 * center;
	float maxOcc = center;
	vec3 offs[4] = vec3[4](T, -T, B, -B);
	for (int i = 0; i < 4; i++) {
		vec3 Pi = P_v + offs[i] * blurRadius;
		float o = ssabssContactOcclusion(depthTexture, projectionMatrix, Pi, N_v, lightPos_v, maxDist, s, noise, uvMin, uvMax, thicknessBase);
		sum += 0.15 * o;
		maxOcc = max(maxOcc, o);
	}
	// darkBias is the weight of the darkest sample against the weighted mean (weight 1): 2 = leans dark 2 to 1, 0 = plain mean
	return (sum + darkBias * maxOcc) / (1.0 + darkBias);
}

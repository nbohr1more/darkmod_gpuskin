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

// Conservative-style expansion of shadow caster triangles (r_shadowMapCullFrontExpand).
//
// Every triangle is grown by u_expandNdc (in NDC units, i.e. N shadow map texels) in all directions,
// so that every shadow map texel gets the nearest depth of the triangles within N texels around it.
// This has the same effect as taking the minimum depth over a footprint of texels when the shadow map is read,
// but it is paid once at rasterization time instead of every time a shadow is looked up.
// The goal is to close light leaks at the seams between cull-front shadow casters.
//
// The method is the one of "Conservative Rasterization" (GPU Gems 2, chapter 42):
// edge planes are computed in homogeneous space, shifted by the expansion, and intersected again.
// Attributes of the new vertices (depth, texcoords, clip distances) are extrapolated from the original triangle.
// The triangle is first clipped to the near plane of the cube face, because the method needs positive w.

layout (triangles) in;
layout (triangle_strip, max_vertices = 4) out;

uniform vec2 u_expandNdc;	// expansion radius along X and Y, in NDC units (note: texels are not square in NDC, the viewport is 6 faces wide)
uniform float u_maxLightDistance;
uniform vec3 u_viewOrigin;			// position of the player in world space
uniform vec2 u_expandFalloff;		// expansion is full within x units from the player, fades out to nothing at y units (y = 0: no falloff)

// the shift of a vertex is limited to this many radii (measured separately along X and Y, then combined):
// at sharp angles the intersection of shifted edges goes very far (spikes). A right angle needs 1.41
#define MITER_LIMIT 2.5
// triangles whose area in NDC (relative to w0 * w1 * w2) is below this are not expanded
#define DEGENERATE_EPS 1e-8

in vec2 vs_texCoord[];
flat in int vs_face[];
in vec3 vs_worldPos[];
out vec2 texCoord;
out float gl_ClipDistance[5];

// distance of the near plane of cube face, see shadow_map.vert.glsl (gl_Position.w is the distance, z = w - 2)
const float NEAR_DIST = 1.0;

// must be the same as in shadow_map.vert.glsl
const float clipEps = 0e-2;
const vec4 ClipPlanes[4] = vec4[4] (
	vec4(1, 0, -1, clipEps),
	vec4(-1, 0, -1, clipEps),
	vec4(0, 1, -1, clipEps),
	vec4(0, -1, -1, clipEps)
);

vec4 clipPos[3];
vec2 expandNdc;		// actual expansion radius of this triangle
float minDist, maxDist;		// range of distances of the original triangle

// polygon (after clipping by near plane), vertices are described by homogeneous barycentric coordinates in the original triangle
vec3 lam[4];
vec3 target[4];		// where to move the vertex, as homogeneous (x, y, w); w = 0 means: don't move it
int polyCount;

// distance from point to triangle (Ericson, "Real-Time Collision Detection")
float distPointTriangle(vec3 p, vec3 a, vec3 b, vec3 c) {
	vec3 ab = b - a, ac = c - a, ap = p - a;
	float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0)
		return length(ap);
	vec3 bp = p - b;
	float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3)
		return length(bp);
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
		return length(p - (a + ab * (d1 / (d1 - d3))));
	vec3 cp = p - c;
	float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6)
		return length(cp);
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
		return length(p - (a + ac * (d2 / (d2 - d6))));
	float va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
		return length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
	float sum = va + vb + vc;
	if (abs(sum) < 1e-12)
		return min(length(ap), min(length(bp), length(cp)));
	return length(p - (a + ab * (vb / sum) + ac * (vc / sum)));
}

void emitIndex(int i) {
	vec3 l = lam[i];
	vec3 t = target[i];
	float s = l.x + l.y + l.z;
	vec3 b = l / s;
	texCoord = b.x * vs_texCoord[0] + b.y * vs_texCoord[1] + b.z * vs_texCoord[2];
	if (t.z <= 0.0) {
		// not moved: exact point of the original triangle (or of its near plane clipping)
		gl_Position = (l.x * clipPos[0] + l.y * clipPos[1] + l.z * clipPos[2]) / s;
		for (int k = 0; k < 5; k++)
			gl_ClipDistance[k] = b.x * gl_in[0].gl_ClipDistance[k] + b.y * gl_in[1].gl_ClipDistance[k] + b.z * gl_in[2].gl_ClipDistance[k];
	} else {
		// moved in XY: the depth of the plane can change extremely fast for triangles which are almost parallel to view direction,
		// so the depth is limited to the range of depths of the original triangle: no spikes in front of the geometry
		float d = clamp(t.z / s, minDist, maxDist);
		vec2 ndc = t.xy / t.z;
		// see shadow_map.vert.glsl: w = d, z = d - 2
		gl_Position = vec4(ndc * d, d - 2.0, d);
		// clip distances are computed exactly for the new position (extrapolation is unstable for thin triangles)
		// inverse of the transformation in shadow_map.vert.glsl:  x = d * (6 * ndc.x + 5 - 2 * face),  y = d * ndc.y,  z = -d
		vec4 fragPos = vec4(d * (6.0 * ndc.x + 5.0 - 2.0 * float(vs_face[0])), d * ndc.y, -d, 1.0);
		for (int k = 0; k < 4; k++)
			gl_ClipDistance[k] = dot(fragPos, ClipPlanes[k]);
		gl_ClipDistance[4] = u_maxLightDistance - d;
	}
	EmitVertex();
}

void emitPolygon() {
	if (polyCount == 3) {
		emitIndex(0);
		emitIndex(1);
		emitIndex(2);
	} else {
		// convex quad as a triangle strip
		emitIndex(0);
		emitIndex(1);
		emitIndex(3);
		emitIndex(2);
	}
	EndPrimitive();
}

// Cheap rejection, done before any of the expensive math below.
// Every triangle is drawn into all 6 cube faces, and most of them lie completely outside of 5 of the faces.
// Such triangle is rejected if it stays outside even after the largest possible expansion:
//   * clip planes: all vertices are further than the maximum shift (MITER_LIMIT radii) behind the same clip plane, measured in NDC
//     of the face (clip distance divided by w is NDC + 1). Vertices at w <= 0 are not trusted, nothing is rejected then.
//   * far plane: expanded vertices keep depth within the range of the original ones, so if the nearest vertex is behind it, everything is
bool isOutsideOfFace() {
	vec3 w = vec3(gl_in[0].gl_Position.w, gl_in[1].gl_Position.w, gl_in[2].gl_Position.w);
	float minW = min(w.x, min(w.y, w.z));
	if (minW > u_maxLightDistance)
		return true;
	if (minW <= 0.0)
		return false;
	// u_expandNdc is in NDC of all 6 faces: X is 6 times narrower than in the NDC of one face
	vec2 margin = MITER_LIMIT * vec2(6.0 * u_expandNdc.x, u_expandNdc.y);
	for (int k = 0; k < 4; k++) {
		float m = k < 2 ? margin.x : margin.y;
		bool outside = true;
		for (int i = 0; i < 3; i++)
			outside = outside && gl_in[i].gl_ClipDistance[k] < -m * w[i];
		if (outside)
			return true;
	}
	return false;
}

void main() {
	if (isOutsideOfFace())
		return;

	clipPos[0] = gl_in[0].gl_Position;
	clipPos[1] = gl_in[1].gl_Position;
	clipPos[2] = gl_in[2].gl_Position;
	minDist = max(min(clipPos[0].w, min(clipPos[1].w, clipPos[2].w)), NEAR_DIST);
	maxDist = max(clipPos[0].w, max(clipPos[1].w, clipPos[2].w));

	// clip the triangle by near plane: keep what has distance >= NEAR_DIST (at most 4 vertices remain)
	vec3 f = vec3(clipPos[0].w, clipPos[1].w, clipPos[2].w) - vec3(NEAR_DIST);
	polyCount = 0;
	vec3 prev = vec3(0.0, 0.0, 1.0);
	float fPrev = f.z;
	for (int i = 0; i < 3; i++) {
		vec3 cur = vec3(float(i == 0), float(i == 1), float(i == 2));
		float fCur = f[i];
		if ((fCur >= 0.0) != (fPrev >= 0.0))
			lam[polyCount++] = mix(prev, cur, fPrev / (fPrev - fCur));
		if (fCur >= 0.0)
			lam[polyCount++] = cur;
		prev = cur;
		fPrev = fCur;
	}
	if (polyCount < 3)
		return;
	for (int i = 0; i < 4; i++)
		target[i] = vec3(0.0);

	// the expansion fades out with the distance from the player: leaks matter most near the camera
	float falloff = 1.0;
	if (u_expandFalloff.y > 0.0) {
		float dist = distPointTriangle(u_viewOrigin, vs_worldPos[0], vs_worldPos[1], vs_worldPos[2]);
		falloff = 1.0 - smoothstep(u_expandFalloff.x, u_expandFalloff.y, dist);
	}
	expandNdc = u_expandNdc * falloff;
	if (falloff < 0.1) {
		// too small to matter: pass the triangle through
		emitPolygon();
		return;
	}

	mat3 A = mat3(clipPos[0].xyw, clipPos[1].xyw, clipPos[2].xyw);
	float det = determinant(A);
	if (abs(det) <= DEGENERATE_EPS * abs(clipPos[0].w * clipPos[1].w * clipPos[2].w)) {
		// degenerate in screen space: has no fragments anyway
		emitPolygon();
		return;
	}
	mat3 Ainv = inverse(A);
	int n = polyCount;

	// homogeneous (x, y, w) of polygon vertices
	vec3 q[4];
	for (int i = 0; i < n; i++)
		q[i] = A * lam[i];

	// edge planes, interior is on the negative side, shifted outwards by the support function of the expansion box
	float orient = det > 0.0 ? -1.0 : 1.0;
	vec3 E[4];
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		E[i] = orient * cross(q[i], q[j]);
		E[i].z -= dot(expandNdc, abs(E[i].xy));
	}

	// the new vertex is the intersection of two neighboring shifted edges
	for (int i = 0; i < n; i++) {
		vec3 hit = cross(E[(i + n - 1) % n], E[i]);
		if (abs(hit.z) <= 1e-12 * (abs(hit.x) + abs(hit.y)))
			continue;
		vec2 ndcOld = q[i].xy / q[i].z;
		vec2 shift = hit.xy / hit.z - ndcOld;
		// texels are not square in NDC: measure the shift in units of the expansion radius along each axis
		float len = length(shift / expandNdc);
		if (len > MITER_LIMIT)
			shift *= MITER_LIMIT / len;
		vec3 t = vec3((ndcOld + shift) * q[i].z, q[i].z);
		vec3 l = Ainv * t;
		// lambda of valid points sums to a positive value, otherwise keep the vertex where it was
		if (l.x + l.y + l.z > 1e-6) {
			lam[i] = l;
			target[i] = t;
		}
	}
	emitPolygon();
}

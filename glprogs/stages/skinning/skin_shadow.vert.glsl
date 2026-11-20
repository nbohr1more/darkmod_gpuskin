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

#pragma tdm_include "tdm_utils.glsl"

// Stencil shadow volume of a GPU-skinned surface, generated from scratch by the vertex shader.
// It does what R_CreateVertexProgramTurboShadowVolume does on CPU, but without any CPU work:
//   * a quad (6 vertices) for every silhouette edge: the edge between a triangle facing the light and one that does not
//   * for every triangle facing away from the light: a cap at its own position (6 vertices = 3 + 3 extruded to infinity)
// The draw has no vertex attributes and no index buffer: gl_VertexID selects the edge or the triangle.
// Edges which are not silhouettes and triangles facing the light become degenerate (zero-area) primitives.
// Index and winding formulas are copied from the CPU code, vertex "idx" is: ( vertex << 1 ) | extrude.

uniform mat4 u_projectionMatrix;
uniform mat4 u_modelViewMatrix;
uniform vec4 u_localLightOrigin;

uniform samplerBuffer u_verts;		// dynamic vertex buffer as floats: skinned idDrawVert (15 floats per vertex)
uniform isamplerBuffer u_silEdges;	// ivec4 per edge: triangle1, triangle2, vertex1, vertex2
uniform isamplerBuffer u_indexes;	// triangle vertex indexes
uniform int u_vertBase;				// index of the first float of the first vertex of the surface in u_verts
uniform int u_numEdges;
uniform int u_numTris;

vec3 getPos(int v) {
	int b = u_vertBase + v * 15;
	return vec3(texelFetch(u_verts, b).x, texelFetch(u_verts, b + 1).x, texelFetch(u_verts, b + 2).x);
}

// the same as idPlane(v0, v1, v2).Distance(lightOrigin) >= 0 (only the sign matters, so no normalization)
bool isFacing(int tri) {
	if (tri >= u_numTris)
		return true;	// dangling edge
	vec3 p0 = getPos(texelFetch(u_indexes, tri * 3 + 0).x);
	vec3 p1 = getPos(texelFetch(u_indexes, tri * 3 + 1).x);
	vec3 p2 = getPos(texelFetch(u_indexes, tri * 3 + 2).x);
	vec3 n = cross(p0 - p1, p2 - p1);
	return dot(n, u_localLightOrigin.xyz - p1) >= 0.0;
}

vec4 shadowVertex(int idx) {
	vec3 p = getPos(idx >> 1);
	if ((idx & 1) != 0) {
		// project vertex position to infinity
		return vec4(p - u_localLightOrigin.xyz, 0.0);
	}
	return vec4(p, 1.0);
}

void main() {
	int vid = gl_VertexID;
	// degenerate by default
	gl_Position = vec4(0.0);
	int idx = -1;

	if (vid < 6 * u_numEdges) {
		int e = vid / 6;
		int k = vid - e * 6;
		ivec4 se = texelFetch(u_silEdges, e);
		int f1 = isFacing(se.x) ? 1 : 0;
		int f2 = isFacing(se.y) ? 1 : 0;
		if (f1 != f2) {
			int v1 = se.z << 1;
			int v2 = se.w << 1;
			if (k == 0) idx = v1;
			else if (k == 1) idx = v2 ^ f1;
			else if (k == 2) idx = v2 ^ f2;
			else if (k == 3) idx = v1 ^ f2;
			else if (k == 4) idx = v1 ^ f1;
			else idx = v2 ^ 1;
		}
	} else {
		int c = vid - 6 * u_numEdges;
		int tri = c / 6;
		int k = c - tri * 6;
		if (tri < u_numTris && !isFacing(tri)) {
			int i0 = texelFetch(u_indexes, tri * 3 + 0).x << 1;
			int i1 = texelFetch(u_indexes, tri * 3 + 1).x << 1;
			int i2 = texelFetch(u_indexes, tri * 3 + 2).x << 1;
			if (k == 0) idx = i2;
			else if (k == 1) idx = i1;
			else if (k == 2) idx = i0;
			else if (k == 3) idx = i0 ^ 1;
			else if (k == 4) idx = i1 ^ 1;
			else idx = i2 ^ 1;
		}
	}

	if (idx >= 0) {
		gl_Position = objectPosToClip(shadowVertex(idx), u_modelViewMatrix, u_projectionMatrix);
	}
}

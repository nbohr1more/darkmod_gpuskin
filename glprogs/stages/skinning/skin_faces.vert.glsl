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

// GPU skinning, pass 2 (per triangle): computes unit normal and unit tangents of every skinned triangle.
// This is what idSIMD::DeriveTangents does per triangle on CPU. Results are gathered per vertex by pass 3
// (skin_tangents.vert.glsl), so every triangle is computed only once instead of once per vertex.
// Degenerate triangles give zero vectors, which contribute nothing to the sums.

uniform samplerBuffer u_scratch;	// output of pass 1: two vec4 per vertex
uniform isamplerBuffer u_indexes;	// three vertex indexes per triangle
uniform int u_scratchBase;			// index of the first vertex of this batch in u_scratch
uniform int u_numVerts;				// vertices per instance (instance k starts at u_scratchBase + k * u_numVerts)

out vec4 tf_n;
out vec4 tf_t0;
out vec4 tf_t1;

void getVert(int index, out vec3 pos, out vec2 st) {
	int texel = (u_scratchBase + gl_InstanceID * u_numVerts + index) * 2;
	vec4 a = texelFetch(u_scratch, texel);
	vec4 b = texelFetch(u_scratch, texel + 1);
	pos = a.xyz;
	st = vec2(a.w, b.x);
}

void main() {
	int tri = gl_VertexID;
	int i0 = texelFetch(u_indexes, tri * 3 + 0).x;
	int i1 = texelFetch(u_indexes, tri * 3 + 1).x;
	int i2 = texelFetch(u_indexes, tri * 3 + 2).x;
	vec3 pa, pb, pc;
	vec2 sa, sb, sc;
	getVert(i0, pa, sa);
	getVert(i1, pb, sb);
	getVert(i2, pc, sc);

	vec3 d0 = pb - pa;
	vec3 d1 = pc - pa;
	vec2 e0 = sb - sa;
	vec2 e1 = sc - sa;

	vec3 n = vec3(0.0), t0 = vec3(0.0), t1 = vec3(0.0);

	vec3 nn = vec3(
		d1.y * d0.z - d1.z * d0.y,
		d1.z * d0.x - d1.x * d0.z,
		d1.x * d0.y - d1.y * d0.x
	);
	float nl = dot(nn, nn);
	if (nl >= 1e-30) {
		n = nn * inversesqrt(nl);

		// sign of texture area
		float area = e0.x * e1.y - e0.y * e1.x;
		float sgn = (floatBitsToUint(area) >> 31u) != 0u ? -1.0 : 1.0;

		vec3 tt0 = d0 * e1.y - d1 * e0.y;
		float l0 = dot(tt0, tt0);
		if (l0 > 1e-30)
			t0 = tt0 * (inversesqrt(l0) * sgn);

		vec3 tt1 = d1 * e0.x - d0 * e1.x;
		float l1 = dot(tt1, tt1);
		if (l1 > 1e-30)
			t1 = tt1 * (inversesqrt(l1) * sgn);
	}

	tf_n = vec4(n, 0.0);
	tf_t0 = vec4(t0, 0.0);
	tf_t1 = vec4(t1, 0.0);
	gl_Position = vec4(0.0);
}

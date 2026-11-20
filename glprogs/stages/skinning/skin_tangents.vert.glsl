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

// GPU skinning, pass 3 (per vertex): builds final idDrawVert (60 bytes) of every vertex of an MD5 mesh with
// transform feedback. Normals and tangents are gathered from the per-triangle results of pass 2
// (skin_faces.vert.glsl), exactly like R_DeriveTangents does on CPU
// (idSIMD::DeriveTangents + dup vertex normal merging + NormalizeTangents):
//   * vertex sums unit normal and tangents of all triangles it belongs to
//   * normal also adds the triangles of one "other" vertex with the same position (the dupVerts merge, including its
//     asymmetric result for 3+ vertices at one position, see idMD5Mesh::BuildSkinnedVerts)
//   * then the vectors are normalized and tangents are projected onto the normal plane
// Triangle lists per vertex are precomputed on load (see idMD5Mesh::BuildSkinnedVerts).

in vec2 attr_TexCoord;
in vec4 attr_Color;
in vec3 attr_Normal;		// rest pose, used only if vertex has no triangles
in vec3 attr_Tangent;
in vec3 attr_Bitangent;

uniform samplerBuffer u_scratch;	// output of pass 1: two vec4 per vertex
uniform isamplerBuffer u_vertInfo;	// ivec4 per vertex: (ownStart, ownCount, otherStart, otherCount) into u_lists
uniform isamplerBuffer u_lists;		// triangle ids
uniform samplerBuffer u_faces;		// output of pass 2: three vec4 per triangle (normal, tangent0, tangent1)
uniform int u_scratchBase;			// index of the first vertex of this batch in u_scratch
uniform int u_faceBase;				// index of the first triangle of this batch in u_faces
uniform int u_numVerts;				// vertices per instance
uniform int u_numTris;				// triangles per instance

// layout and order must match idDrawVert
out vec3 tf_xyz;
out vec2 tf_st;
out vec3 tf_normal;
out vec3 tf_tangent0;
out vec3 tf_tangent1;
flat out uint tf_color;

void main() {
	int v = gl_VertexID;

	int inst = gl_InstanceID;
	vec3 pos = texelFetch(u_scratch, (u_scratchBase + inst * u_numVerts + v) * 2).xyz;

	ivec4 info = texelFetch(u_vertInfo, v);
	vec3 nSum = vec3(0.0), t0Sum = vec3(0.0), t1Sum = vec3(0.0);
	for (int i = 0; i < info.y; i++) {
		int face = (u_faceBase + inst * u_numTris + texelFetch(u_lists, info.x + i).x) * 3;
		nSum += texelFetch(u_faces, face + 0).xyz;
		t0Sum += texelFetch(u_faces, face + 1).xyz;
		t1Sum += texelFetch(u_faces, face + 2).xyz;
	}
	// triangles of the other vertex at the same position (only normals are merged between them)
	for (int i = 0; i < info.w; i++) {
		int face = (u_faceBase + inst * u_numTris + texelFetch(u_lists, info.z + i).x) * 3;
		nSum += texelFetch(u_faces, face + 0).xyz;
	}

	vec3 n = attr_Normal;
	float nl = dot(nSum, nSum);
	if (nl > 1e-30)
		n = nSum * inversesqrt(nl);

	vec3 t0 = attr_Tangent;
	t0Sum -= dot(t0Sum, n) * n;
	float l0 = dot(t0Sum, t0Sum);
	if (l0 > 1e-30)
		t0 = t0Sum * inversesqrt(l0);

	vec3 t1 = attr_Bitangent;
	t1Sum -= dot(t1Sum, n) * n;
	float l1 = dot(t1Sum, t1Sum);
	if (l1 > 1e-30)
		t1 = t1Sum * inversesqrt(l1);

	tf_xyz = pos;
	tf_st = attr_TexCoord;
	tf_normal = n;
	tf_tangent0 = t0;
	tf_tangent1 = t1;
	// byte color (little endian: color[0] is the lowest byte)
	uvec4 c = uvec4(clamp(attr_Color, 0.0, 1.0) * 255.0 + 0.5);
	tf_color = c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
	gl_Position = vec4(0.0);
}

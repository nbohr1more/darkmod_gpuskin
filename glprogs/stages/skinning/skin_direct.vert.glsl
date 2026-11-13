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

// GPU skinning for surfaces which are drawn without lighting (shadow maps, unlit materials):
// only positions are needed, so skin them and write final idDrawVert (60 bytes) directly.
// Normals and tangents are left as in rest pose and must not be used.

in vec3 attr_WeightPos0;
in vec3 attr_WeightPos1;
in vec3 attr_WeightPos2;
in vec3 attr_WeightPos3;
in uvec4 attr_JointIndices;
in vec4 attr_JointWeights;
in vec2 attr_TexCoord;
in vec4 attr_Color;
in vec3 attr_Normal;
in vec3 attr_Tangent;
in vec3 attr_Bitangent;

uniform samplerBuffer u_joints;		// all joint matrices of the frame: 3 texels (rows: xyz = rotation row, w = translation) per joint
uniform isamplerBuffer u_instances;	// first joint of every instance (surface) in u_joints
uniform int u_instanceBase;			// index of the first instance of this batch in u_instances

// layout and order must match idDrawVert
out vec3 tf_xyz;
out vec2 tf_st;
out vec3 tf_normal;
out vec3 tf_tangent0;
out vec3 tf_tangent1;
flat out uint tf_color;

vec3 skinWeight(int jointBase, int i, vec3 weightPos) {
	float w = attr_JointWeights[i];
	int j = (jointBase + int(attr_JointIndices[i])) * 3;
	vec4 p = vec4(weightPos, 1.0);
	return w * vec3(dot(texelFetch(u_joints, j + 0), p), dot(texelFetch(u_joints, j + 1), p), dot(texelFetch(u_joints, j + 2), p));
}

void main() {
	int jointBase = texelFetch(u_instances, u_instanceBase + gl_InstanceID).x;
	tf_xyz = skinWeight(jointBase, 0, attr_WeightPos0)
		+ skinWeight(jointBase, 1, attr_WeightPos1)
		+ skinWeight(jointBase, 2, attr_WeightPos2)
		+ skinWeight(jointBase, 3, attr_WeightPos3);
	tf_st = attr_TexCoord;
	tf_normal = attr_Normal;
	tf_tangent0 = attr_Tangent;
	tf_tangent1 = attr_Bitangent;
	uvec4 c = uvec4(clamp(attr_Color, 0.0, 1.0) * 255.0 + 0.5);
	tf_color = c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
	gl_Position = vec4(0.0);
}

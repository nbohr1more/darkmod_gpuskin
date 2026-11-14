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

// GPU skinning, pass 1: skins vertex positions of one MD5 mesh with transform feedback.
// Exactly like MD5 CPU skinning: position = sum( weight_k * joint_k * offset_k ).
// Since the joint matrices are (current joint) * (inverse default pose), every weight uses
// the rest-pose position seen through that weight (attr_WeightPosK = default joint * offset).
// Output is read by pass 2 (skin_tangents.vert.glsl) to derive normals/tangents from skinned triangles.

in vec3 attr_WeightPos0;
in vec3 attr_WeightPos1;
in vec3 attr_WeightPos2;
in vec3 attr_WeightPos3;
in uvec4 attr_JointIndices;
in vec4 attr_JointWeights;
in vec2 attr_TexCoord;

uniform samplerBuffer u_joints;		// all joint matrices of the frame: 3 texels (rows: xyz = rotation row, w = translation) per joint
uniform isamplerBuffer u_instances;	// first joint of every instance (surface) in u_joints
uniform int u_instanceBase;			// index of the first instance of this batch in u_instances

out vec4 tf_a;	// position.xyz, st.x
out vec4 tf_b;	// st.y, unused

vec3 skinWeight(int jointBase, int i, vec3 weightPos) {
	float w = attr_JointWeights[i];
	int j = (jointBase + int(attr_JointIndices[i])) * 3;
	vec4 p = vec4(weightPos, 1.0);
	return w * vec3(dot(texelFetch(u_joints, j + 0), p), dot(texelFetch(u_joints, j + 1), p), dot(texelFetch(u_joints, j + 2), p));
}

void main() {
	// instanced: one instance per surface, instances are written to the output one after another
	int jointBase = texelFetch(u_instances, u_instanceBase + gl_InstanceID).x;
	// weights are sorted in descending order, so the first zero weight means the rest are zero too
	vec3 pos = skinWeight(jointBase, 0, attr_WeightPos0);
	if (attr_JointWeights.y > 0.0) {
		pos += skinWeight(jointBase, 1, attr_WeightPos1);
		if (attr_JointWeights.z > 0.0) {
			pos += skinWeight(jointBase, 2, attr_WeightPos2);
			if (attr_JointWeights.w > 0.0)
				pos += skinWeight(jointBase, 3, attr_WeightPos3);
		}
	}
	tf_a = vec4(pos, attr_TexCoord.x);
	tf_b = vec4(attr_TexCoord.y, 0.0, 0.0, 0.0);
	gl_Position = vec4(0.0);
}

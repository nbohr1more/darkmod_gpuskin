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
#version 140

// Shadow map fragment shader with clamped slope-scaled depth bias (emulates glPolygonOffsetClamp, which is GL 4.6 / GL_EXT_polygon_offset_clamp).
// The bias is applied to linear distance along the cube face axis (in world units), not to raw depth buffer units.
// Depth stored in the shadow map is: 1 - 1 / distance  (see shadow_map.vert.glsl and tdm_shadowmaps.glsl).

uniform sampler2D u_opaqueTexture;
uniform float u_alphaTest;
// x = slope factor, y = max slope-dependent bias (world units), z = constant bias (world units)
// positive bias moves the stored surface towards the light
uniform vec3 u_depthBias;
in vec2 texCoord;

void main() {
	// derivatives must be taken before discard
	float dist = 1.0 / (1.0 - gl_FragCoord.z);
	float slope = max(abs(dFdx(dist)), abs(dFdy(dist)));

	if (u_alphaTest >= 0) {
		vec4 tex = texture(u_opaqueTexture, texCoord);
		if (tex.a <= u_alphaTest)
			discard;
	}

	float bias = clamp(u_depthBias.x * slope, -u_depthBias.y, u_depthBias.y) + u_depthBias.z;
	float biasedDist = max(dist - bias, 1.0001);	// 1.0 is the near plane
	gl_FragDepth = 1.0 - 1.0 / biasedDist;
}

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

#ifndef __DRAWVERT_H__
#define __DRAWVERT_H__

/*
===============================================================================

	Draw Vertex.

===============================================================================
*/

class idDrawVert {
public:
	idVec3			xyz;
	idVec2			st;
	idVec3			normal;
	idVec3			tangents[2];
	byte			color[4] = {0};
#if 0 // was MACOS_X see comments concerning DRAWVERT_PADDED in Simd_Altivec.h 
	float			padding;
#endif
	float			operator[]( const int index ) const;
	float &			operator[]( const int index );

	void			Clear( void );

	void			Lerp( const idDrawVert &a, const idDrawVert &b, const float f );
	void			LerpAll( const idDrawVert &a, const idDrawVert &b, const float f );

	void			Normalize( void );
	// stgatilov: this normalization scales normal/tangents by common factor, so that normal becomes unit length.
	void			ScaleToUnitNormal( void );

	void			SetColor( dword color );
	dword			GetColor( void ) const;
};

ID_FORCE_INLINE float idDrawVert::operator[]( const int index ) const {
	assert( index >= 0 && index < 5 );
	return ((float *)(&xyz))[index];
}
ID_FORCE_INLINE float	&idDrawVert::operator[]( const int index ) {
	assert( index >= 0 && index < 5 );
	return ((float *)(&xyz))[index];
}

/*
===============================================================================

	Skinned Draw Vertex (GPU skinning)

	Rest-pose idDrawVert at full float precision, followed by joint indices and
	joint weights used by the vertex shader to blend joint matrices.
	Position, normal and tangents are in the default (bind) pose.
	Layout is a strict superset of idDrawVert, so an unskinned shader can read it.

===============================================================================
*/
class idDrawVertSkinned {
public:
	idDrawVert		vert;				// 60 bytes, rest pose
	byte			jointIndices[4];	// 4 bytes, unused entries have zero weight
	float			jointWeights[4];	// 16 bytes, MD5 weights (sum ~1.0), zero for unused entries
	// Rest-pose position as seen through each weight: (default pose of joint k) * (MD5 weight offset k).
	// The shader computes sum_k( w_k * skinMatrix_k * weightPos[k] ), which is exactly the MD5 formula
	// sum_k( w_k * currentJoint_k * offset_k ), since skinMatrix = currentJoint * inverse(defaultPose).
	idVec3			weightPos[4];		// 48 bytes
};
static_assert( sizeof( idDrawVertSkinned ) == sizeof( idDrawVert ) + 4 + 16 + 48, "idDrawVertSkinned must have no padding" );

// maximum number of joints addressable by the byte joint index and the joint uniform block
const int MAX_GPU_SKINNING_JOINTS = 256;
// number of vec4 rows per joint (3x4 matrix)
const int GPU_SKINNING_ROWS_PER_JOINT = 3;

ID_INLINE void idDrawVert::Clear( void ) {
	xyz.Zero();
	st.Zero();
	normal.Zero();
	tangents[0].Zero();
	tangents[1].Zero();
	color[0] = color[1] = color[2] = color[3] = 0;
}

ID_INLINE void idDrawVert::Lerp( const idDrawVert &a, const idDrawVert &b, const float f ) {
	xyz = a.xyz + f * ( b.xyz - a.xyz );
	st = a.st + f * ( b.st - a.st );
}

ID_INLINE void idDrawVert::LerpAll( const idDrawVert &a, const idDrawVert &b, const float f ) {
	xyz = a.xyz + f * ( b.xyz - a.xyz );
	st = a.st + f * ( b.st - a.st );
	normal = a.normal + f * ( b.normal - a.normal );
	tangents[0] = a.tangents[0] + f * ( b.tangents[0] - a.tangents[0] );
	tangents[1] = a.tangents[1] + f * ( b.tangents[1] - a.tangents[1] );
	color[0] = (byte)( a.color[0] + f * ( b.color[0] - a.color[0] ) );
	color[1] = (byte)( a.color[1] + f * ( b.color[1] - a.color[1] ) );
	color[2] = (byte)( a.color[2] + f * ( b.color[2] - a.color[2] ) );
	color[3] = (byte)( a.color[3] + f * ( b.color[3] - a.color[3] ) );
}

ID_FORCE_INLINE void idDrawVert::SetColor( dword color ) {
	*reinterpret_cast<dword *>(this->color) = color;
}

ID_FORCE_INLINE dword idDrawVert::GetColor( void ) const {
	return *reinterpret_cast<const dword *>(this->color);
}

ID_INLINE void idDrawVert::ScaleToUnitNormal( void ) {
	float invNormLen = idMath::RSqrt(normal.LengthSqr());
	if (invNormLen == 1.0f) return;
	normal *= invNormLen;
	tangents[0] *= invNormLen;
	tangents[1] *= invNormLen;
}


#endif /* !__DRAWVERT_H__ */

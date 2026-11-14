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

#include "precompiled.h"
#pragma hdrstop



#include "renderer/tr_local.h"
#include "renderer/resources/Model_local.h"
#include "renderer/backend/GpuSkinning.h"

static const char *MD5_SnapshotName = "_MD5_Snapshot_";


/***********************************************************************

	idMD5Mesh

***********************************************************************/

static int c_numVerts = 0;
static int c_numWeights = 0;
static int c_numWeightJoints = 0;

typedef struct vertexWeight_s {
	int							vert;
	int							joint;
	idVec3						offset;
	float						jointWeight;
} vertexWeight_t;

/*
====================
idMD5Mesh::idMD5Mesh
====================
*/
idMD5Mesh::idMD5Mesh() {
	scaledWeights	= NULL;
	weightIndex		= NULL;
	shader			= NULL;
	numTris			= 0;
	deformInfo		= NULL;
	surfaceNum		= 0;
}

/*
====================
idMD5Mesh::~idMD5Mesh
====================
*/
idMD5Mesh::~idMD5Mesh() {
	Mem_Free16( scaledWeights );
	Mem_Free16( weightIndex );
	Mem_Free16( skinFillWeights );
	Mem_Free16( skinFillIndex );
	if ( deformInfo ) {
		R_FreeDeformInfo( deformInfo );
		deformInfo = NULL;
	}
}

/*
====================
idMD5Mesh::ParseMesh
====================
*/
void idMD5Mesh::ParseMesh( idLexer &parser, int numJoints, const idJointMat *joints, idBounds *jointBounds ) {
	idToken		token;
	idToken		name;
	int			num;
	int			count;
	int			jointnum;
	idStr		shaderName;
	int			i, j;
	idList<int>	tris;
	idList<int>	firstWeightForVertex;
	idList<int>	numWeightsForVertex;
	int			maxweight;
	idList<vertexWeight_t> tempWeights;

	parser.ExpectTokenString( "{" );

	//
	// parse name
	//
	if ( parser.CheckTokenString( "name" ) ) {
		parser.ReadToken( &name );
	}

	//
	// parse shader
	//
	parser.ExpectTokenString( "shader" );

	parser.ReadToken( &token );
	shaderName = token;

    shader = declManager->FindMaterial( shaderName );

	//
	// parse texture coordinates
	//
	parser.ExpectTokenString( "numverts" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %s", token.c_str() );
	}

	texCoords.SetNum( count );
	firstWeightForVertex.SetNum( count );
	numWeightsForVertex.SetNum( count );

	numWeights = 0;
	maxweight = 0;
	for( i = 0; i < texCoords.Num(); i++ ) {
		parser.ExpectTokenString( "vert" );
		parser.ParseInt();

		parser.Parse1DMatrix( 2, texCoords[ i ].ToFloatPtr() );

		firstWeightForVertex[ i ]	= parser.ParseInt();
		numWeightsForVertex[ i ]	= parser.ParseInt();

		if ( !numWeightsForVertex[ i ] ) {
			parser.Error( "Vertex without any joint weights." );
		}

		numWeights += numWeightsForVertex[ i ];
		if ( numWeightsForVertex[ i ] + firstWeightForVertex[ i ] > maxweight ) {
			maxweight = numWeightsForVertex[ i ] + firstWeightForVertex[ i ];
		}
	}

	//
	// parse tris
	//
	parser.ExpectTokenString( "numtris" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %d", count );
	}

	tris.SetNum( count * 3 );
	numTris = count;
	for( i = 0; i < count; i++ ) {
		parser.ExpectTokenString( "tri" );
		parser.ParseInt();

		tris[ i * 3 + 0 ] = parser.ParseInt();
		tris[ i * 3 + 1 ] = parser.ParseInt();
		tris[ i * 3 + 2 ] = parser.ParseInt();
	}

	//
	// parse weights
	//
	parser.ExpectTokenString( "numweights" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %d", count );
	}

	if ( maxweight > count ) {
		parser.Warning( "Vertices reference out of range weights in model (%d of %d weights).", maxweight, count );
	}

	tempWeights.SetNum( count );

	for( i = 0; i < count; i++ ) {
		parser.ExpectTokenString( "weight" );
		parser.ParseInt();

		jointnum = parser.ParseInt();
		if ( ( jointnum < 0 ) || ( jointnum >= numJoints ) ) {
			parser.Error( "Joint Index out of range(%d): %d", numJoints, jointnum );
		}

		tempWeights[ i ].joint			= jointnum;
		tempWeights[ i ].jointWeight	= parser.ParseFloat();

		parser.Parse1DMatrix( 3, tempWeights[ i ].offset.ToFloatPtr() );
	}

	// stgatilov #6099: fill per-joint bounds
	for( i = 0; i < tempWeights.Num(); i++ ) {
		idBounds &jb = jointBounds[tempWeights[i].joint];
		const idVec3 &p = tempWeights[i].offset;
		jb.AddPoint(p);
	}

	// create pre-scaled weights and an index for the vertex/joint lookup
	scaledWeights = (idVec4 *) Mem_Alloc16( numWeights * sizeof( scaledWeights[0] ) );
	weightIndex = (int *) Mem_Alloc16( numWeights * 2 * sizeof( weightIndex[0] ) );
	memset( weightIndex, 0, numWeights * 2 * sizeof( weightIndex[0] ) );
	vertexStarts.SetNum( texCoords.Num() + 1 );

	count = 0;
	for( i = 0; i < texCoords.Num(); i++ ) {
		vertexStarts[i] = count;
		num = firstWeightForVertex[i];
		for( j = 0; j < numWeightsForVertex[i]; j++, num++, count++ ) {
			scaledWeights[count].ToVec3() = tempWeights[num].offset * tempWeights[num].jointWeight;
			scaledWeights[count].w = tempWeights[num].jointWeight;
			weightIndex[count * 2 + 0] = tempWeights[num].joint * sizeof( idJointMat );
		}
		weightIndex[count * 2 - 1] = 1;
	}
	vertexStarts.Last() = count;

	tempWeights.Clear();
	numWeightsForVertex.Clear();
	firstWeightForVertex.Clear();

	parser.ExpectTokenString( "}" );

	// update counters
	c_numVerts += texCoords.Num();
	c_numWeights += numWeights;
	c_numWeightJoints++;
	for ( i = 0; i < numWeights; i++ ) {
		c_numWeightJoints += weightIndex[i*2+1];
	}

	//
	// build the information that will be common to all animations of this mesh:
	// silhouette edge connectivity and normal / tangent generation information
	//
	idDrawVert *verts = (idDrawVert *) _alloca16( texCoords.Num() * sizeof( idDrawVert ) );
	for ( i = 0; i < texCoords.Num(); i++ ) {
		verts[i].Clear();
		verts[i].st = texCoords[i];
	}
	TransformVerts( verts, joints );
	deformInfo = R_BuildDeformInfo( texCoords.Num(), verts, tris.Num(), tris.Ptr(), shader->UseUnsmoothedTangents() );

	// GPU skinning: static vertex buffer with rest pose + joint indices and weights
	BuildSkinnedVerts( numJoints, joints, verts );
}

/*
====================
idMD5Mesh::BuildSkinnedVerts

Builds the static vertex array used for GPU skinning.
Normals and tangents are in the rest (default pose) model space, the shader rotates them with
(current joint * inverse default pose) matrices. Positions are skinned exactly like MD5 does it:
every weight stores its own rest position (default pose joint * weight offset), see idDrawVertSkinned.
Only the 4 largest weights of each vertex are kept (the rest is compensated by scaling the kept ones),
so vertices with more than 4 weights are slightly different from CPU skinning.
====================
*/
void idMD5Mesh::BuildSkinnedVerts( int numJoints, const idJointMat *defaultPose, const idDrawVert *restVerts ) {
	if ( deformInfo == NULL || numJoints > MAX_GPU_SKINNING_JOINTS || deformInfo->numOutputVerts <= 0 ) {
		return;
	}
	if ( deformInfo->dominantTris != NULL ) {
		// materials with "unsmoothedTangents" derive normals from a single triangle (R_DeriveUnsmoothedTangents),
		// this is not implemented on GPU: such meshes are skinned on CPU
		return;
	}
	const int numSrc = deformInfo->numSourceVerts;
	const int numOut = deformInfo->numOutputVerts;

	// rest pose geometry with normals and tangents
	srfTriangles_t *tmp = R_AllocStaticTriSurf();
	tmp->deformedSurface = true;		// indexes etc. are shared with deformInfo
	tmp->numIndexes = deformInfo->numIndexes;
	tmp->indexes = deformInfo->indexes;
	tmp->silIndexes = deformInfo->silIndexes;
	tmp->numMirroredVerts = deformInfo->numMirroredVerts;
	tmp->mirroredVerts = deformInfo->mirroredVerts;
	tmp->numDupVerts = deformInfo->numDupVerts;
	tmp->dupVerts = deformInfo->dupVerts;
	tmp->numSilEdges = deformInfo->numSilEdges;
	tmp->silEdges = deformInfo->silEdges;
	tmp->dominantTris = deformInfo->dominantTris;
	tmp->numVerts = numOut;
	R_AllocStaticTriSurfVerts( tmp, numOut );
	// restVerts are already transformed by the default pose (see ParseMesh)
	for ( int i = 0; i < numSrc; i++ ) {
		tmp->verts[i] = restVerts[i];
	}
	const int mirrorBase = numOut - deformInfo->numMirroredVerts;
	for ( int i = 0; i < deformInfo->numMirroredVerts; i++ ) {
		tmp->verts[mirrorBase + i] = tmp->verts[deformInfo->mirroredVerts[i]];
	}
	// get normals and tangents (face planes are not needed here)
	R_DeriveTangents( tmp, false );

	idDrawVertSkinned *out = (idDrawVertSkinned *) Mem_Alloc16( numOut * sizeof( idDrawVertSkinned ) );
	std::vector<idVec3> fallbackPositions;	// vertices without usable weights

	// choose up to 4 strongest weights for every source vertex
	for ( int i = 0; i < numSrc; i++ ) {
		int bestJoint[4] = { 0, 0, 0, 0 };
		int bestWeightIdx[4] = { -1, -1, -1, -1 };
		float bestWeight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float totalWeight = 0.0f;
		for ( int k = vertexStarts[i]; k < vertexStarts[i + 1]; k++ ) {
			int joint = weightIndex[k * 2] / (int)sizeof( idJointMat );
			float w = scaledWeights[k].w;
			totalWeight += w;
			if ( w <= 0.0f ) {
				continue;
			}
			// insert into the sorted (descending) list
			int pos = 4;
			while ( pos > 0 && w > bestWeight[pos - 1] ) {
				pos--;
			}
			if ( pos >= 4 ) {
				continue;
			}
			for ( int m = 3; m > pos; m-- ) {
				bestJoint[m] = bestJoint[m - 1];
				bestWeight[m] = bestWeight[m - 1];
				bestWeightIdx[m] = bestWeightIdx[m - 1];
			}
			bestJoint[pos] = joint;
			bestWeight[pos] = w;
			bestWeightIdx[pos] = k;
		}
		const float keptWeight = bestWeight[0] + bestWeight[1] + bestWeight[2] + bestWeight[3];
		// MD5 does not renormalize weights, so we don't either (otherwise vertices would differ from CPU skinning).
		// Only if some weights had to be dropped, compensate by scaling the kept ones to the original total.
		const float compensate = ( keptWeight > 1e-6f ) ? totalWeight / keptWeight : 1.0f;

		idDrawVertSkinned &v = out[i];
		v.vert = tmp->verts[i];
		for ( int m = 0; m < 4; m++ ) {
			v.jointIndices[m] = (byte)bestJoint[m];
			v.jointWeights[m] = bestWeight[m] * compensate;
			if ( bestWeightIdx[m] >= 0 ) {
				// offset of the weight in joint space: scaledWeights holds offset * weight
				const int k = bestWeightIdx[m];
				const idVec3 offset = scaledWeights[k].ToVec3() * ( 1.0f / scaledWeights[k].w );
				// the same rest position that TransformVerts would compute for this weight alone
				v.weightPos[m] = defaultPose[bestJoint[m]] * idVec4( offset.x, offset.y, offset.z, 1.0f );
			} else {
				v.weightPos[m].Zero();
			}
		}
		if ( keptWeight <= 1e-6f ) {
			// no usable weights: vertex stays at its rest position (identity-like through root joint)
			v.jointIndices[0] = 0;
			v.jointWeights[0] = 1.0f;
			v.weightPos[0] = v.vert.xyz;
			fallbackPositions.push_back( v.vert.xyz );	// joint 0 bounds must include them
		}
	}
	// mirrored verts share the skinning data of their sources
	for ( int i = 0; i < deformInfo->numMirroredVerts; i++ ) {
		idDrawVertSkinned &v = out[mirrorBase + i];
		v = out[deformInfo->mirroredVerts[i]];
		v.vert = tmp->verts[mirrorBase + i];
	}
	R_ReallyFreeStaticTriSurf( tmp );

	// Per-vertex lists of triangles, so that GPU can derive smooth normals and tangents exactly like
	// R_DeriveTangents does. Tangents sum over the triangles of the vertex. Normals also take part in the
	// merge of vertices with the same position, which is done by this CPU code (see R_DeriveTangents):
	//   dupVerts has a pair (m, c) for every vertex m which has the same position as an earlier vertex c
	//   (c is the first such vertex, so it never appears as m);
	//   for each pair in order:  normal[m] += normal[c]
	//   for each pair in order:  normal[c] = normal[m]
	// With two vertices at the position it just sums both. With more, the result is not symmetric:
	//   every m gets own(m) + own(c), and c gets own(c) + own(last m of c).
	// So in all cases a vertex adds the triangles of exactly one "other" vertex to its normal.
	std::vector<int> other( numOut, -1 );
	std::vector<int> lastMember( numOut, -1 );
	std::vector<char> isMember( numOut, 0 );
	bool badGroups = false;
	for ( int i = 0; i < deformInfo->numDupVerts; i++ ) {
		const int m = deformInfo->dupVerts[i * 2 + 0];
		const int c = deformInfo->dupVerts[i * 2 + 1];
		if ( m < 0 || m >= numOut || c < 0 || c >= numOut || m == c || isMember[m] ) {
			badGroups = true;
			break;
		}
		isMember[m] = 1;
		other[m] = c;
		lastMember[c] = m;
	}
	for ( int i = 0; i < deformInfo->numDupVerts && !badGroups; i++ ) {
		if ( isMember[deformInfo->dupVerts[i * 2 + 1]] ) {
			badGroups = true;	// chain: the first vertex of a group is a duplicate itself, not expected
		}
	}
	if ( badGroups ) {
		common->Warning( "GPU skinning disabled for a mesh with material %s: unsupported vertex duplication", shader ? shader->GetName() : "<none>" );
		Mem_Free16( out );
		return;
	}
	for ( int i = 0; i < numOut; i++ ) {
		if ( lastMember[i] >= 0 ) {
			other[i] = lastMember[i];
		}
	}

	// triangle lists of vertices (CSR)
	std::vector<int> ownStart( numOut + 1, 0 );
	for ( int i = 0; i < deformInfo->numIndexes; i++ ) {
		ownStart[deformInfo->indexes[i] + 1]++;
	}
	for ( int i = 0; i < numOut; i++ ) {
		ownStart[i + 1] += ownStart[i];
	}
	std::vector<int> lists( deformInfo->numIndexes );
	{
		std::vector<int> fill( ownStart.begin(), ownStart.end() - 1 );
		for ( int i = 0; i < deformInfo->numIndexes; i++ ) {
			lists[fill[deformInfo->indexes[i]]++] = i / 3;
		}
	}
	std::vector<int> vertInfo( numOut * 4, 0 );
	for ( int i = 0; i < numOut; i++ ) {
		vertInfo[i * 4 + 0] = ownStart[i];
		vertInfo[i * 4 + 1] = ownStart[i + 1] - ownStart[i];
		if ( other[i] >= 0 ) {
			vertInfo[i * 4 + 2] = ownStart[other[i]];
			vertInfo[i * 4 + 3] = ownStart[other[i] + 1] - ownStart[other[i]];
		}
	}

	deformInfo->skinnedVerts = out;
	deformInfo->numSkinnedTriLists = (int)lists.size();
	deformInfo->skinnedVertInfo = (int *) Mem_Alloc16( vertInfo.size() * sizeof( int ) );
	memcpy( deformInfo->skinnedVertInfo, vertInfo.data(), vertInfo.size() * sizeof( int ) );
	deformInfo->skinnedTriLists = (int *) Mem_Alloc16( std::max<size_t>( lists.size(), 1 ) * sizeof( int ) );
	if ( !lists.empty() ) {
		memcpy( deformInfo->skinnedTriLists, lists.data(), lists.size() * sizeof( int ) );
	}

	// bounds of the mesh from joints: every vertex is a weighted sum of joint-transformed offsets,
	// so it lies in the union of the transformed per-joint bounds (given weights are positive and sum to one)
	{
		idList<idBounds> jb;
		jb.SetNum( numJoints );
		for ( int j = 0; j < numJoints; j++ ) {
			jb[j].Clear();
		}
		for ( int k = 0; k < numWeights; k++ ) {
			if ( scaledWeights[k].w == 0.0f )
				continue;
			const int joint = weightIndex[k * 2] / (int)sizeof( idJointMat );
			if ( joint < 0 || joint >= numJoints )
				continue;
			jb[joint].AddPoint( scaledWeights[k].ToVec3() * ( 1.0f / scaledWeights[k].w ) );
		}
		if ( numJoints > 0 ) {
			for ( const idVec3 &p : fallbackPositions ) {
				jb[0].AddPoint( p );
			}
		}
		skinnedBounds.Clear();
		skinnedBoundsJoints = numJoints;
		for ( int j = 0; j < numJoints; j++ ) {
			if ( jb[j].IsCleared() )
				continue;
			skinBoundsJoint_t e;
			e.joint = j;
			e.center = ( jb[j][0] + jb[j][1] ) * 0.5f;
			e.extents = jb[j][1] - e.center;
			skinnedBounds.Append( e );
		}
	}

	// weights for filling CPU positions on demand (r_skinLazyPositions): same format as scaledWeights/weightIndex,
	// but they are applied with the skinning matrices (see skinJointSet_t) and use the 4 weights of GPU skinning
	{
		std::vector<idVec4> fw;
		std::vector<int> fi;
		for ( int i = 0; i < numSrc; i++ ) {
			const idDrawVertSkinned &sv = out[i];
			const size_t first = fw.size();
			for ( int k = 0; k < 4; k++ ) {
				if ( sv.jointWeights[k] == 0.0f )
					continue;
				const float w = sv.jointWeights[k];
				fw.push_back( idVec4( sv.weightPos[k].x * w, sv.weightPos[k].y * w, sv.weightPos[k].z * w, w ) );
				fi.push_back( sv.jointIndices[k] * (int)sizeof( idJointMat ) );
				fi.push_back( 0 );
			}
			if ( fw.size() == first ) {
				// at least one entry per vertex is required by TransformVerts
				fw.push_back( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
				fi.push_back( 0 );
				fi.push_back( 0 );
			}
			fi[fi.size() - 1] = 1;
		}
		Mem_Free16( skinFillWeights );
		Mem_Free16( skinFillIndex );
		numSkinFillWeights = (int)fw.size();
		skinFillWeights = (idVec4 *) Mem_Alloc16( fw.size() * sizeof( idVec4 ) );
		skinFillIndex = (int *) Mem_Alloc16( fi.size() * sizeof( int ) );
		memcpy( skinFillWeights, fw.data(), fw.size() * sizeof( idVec4 ) );
		memcpy( skinFillIndex, fi.data(), fi.size() * sizeof( int ) );
	}

	GpuSkinning::MeshData data;
	static_assert( sizeof( silEdge_t ) == 4 * sizeof( int ) && sizeof( glIndex_t ) == sizeof( int ), "silEdges are uploaded as ivec4" );
	data.silEdges = deformInfo->silEdges ? (const int *)deformInfo->silEdges : NULL;
	data.numSilEdges = deformInfo->silEdges ? deformInfo->numSilEdges : 0;
	data.verts = out;
	data.numVerts = numOut;
	data.vertInfo = deformInfo->skinnedVertInfo;
	data.triLists = deformInfo->skinnedTriLists;
	data.numTriLists = deformInfo->numSkinnedTriLists;
	data.indexes = deformInfo->indexes;
	data.numIndexes = deformInfo->numIndexes;
	deformInfo->skinnedCache = gpuSkinning.AllocMesh( data );
}

/*
====================
idMD5Mesh::TransformVerts
====================
*/
void idMD5Mesh::TransformVerts( idDrawVert *verts, const idJointMat *entJoints ) const {
	SIMDProcessor->TransformVerts( verts, texCoords.Num(), entJoints, scaledWeights, weightIndex, numWeights );
}

/*
====================
idMD5Mesh::TransformScaledVerts

Special transform to make the mesh seem fat or skinny.  May be used for zombie deaths
====================
*/
void idMD5Mesh::TransformScaledVerts( idDrawVert *verts, const idJointMat *entJoints, float scale ) const {
	idVec4 *tmpScaledWeights = (idVec4 *) _alloca16( numWeights * sizeof( scaledWeights[0] ) );
	//SIMDProcessor->Mul( tmpScaledWeights[0].ToFloatPtr(), scale, this->scaledWeights[0].ToFloatPtr(), numWeights * 4 );
	// DG: for this effect to work, we must scale x, y, z but not w (when also scaling w it just shrinks)
	//     (might not look great with all monsters, depending on their bones etc, esp. faces tend to
	//      be problematic. I don't think this can be fixed here and in the end it's up to the FM author
	//      to use this in a way that looks ok)
	for( int i=0, n=numWeights; i < n; ++i ) {
		const idVec4& sw = this->scaledWeights[i];
		tmpScaledWeights[i].x = sw.x * scale;
		tmpScaledWeights[i].y = sw.y * scale;
		tmpScaledWeights[i].z = sw.z * scale;
		tmpScaledWeights[i].w = sw.w;
	}
	SIMDProcessor->TransformVerts( verts, texCoords.Num(), entJoints, tmpScaledWeights, weightIndex, numWeights );
}

/*
====================
idMD5Mesh::UpdateSurface
====================
*/
void idMD5Mesh::UpdateSurface( const struct renderEntity_s *ent, const idJointMat *entJoints, modelSurface_t *surf, skinJointSet_t *skinJoints ) const {
	int i, base;
	srfTriangles_t *tri;

	if ( r_showDynamic.GetBool() ) {
		tr.pc.c_deformedSurfaces++;
		tr.pc.c_deformedVerts += deformInfo->numOutputVerts;
		tr.pc.c_deformedIndexes += deformInfo->numIndexes;
	}

	surf->material = shader;

	if ( surf->geometry ) {
		// if the number of verts and indexes are the same we can re-use the triangle surface
		// the number of indexes must be the same to assure the correct amount of memory is allocated for the facePlanes
		if ( surf->geometry->numVerts == deformInfo->numOutputVerts && surf->geometry->numIndexes == deformInfo->numIndexes ) {
			R_FreeStaticTriSurfVertexCaches( surf->geometry );
		} else {
			R_FreeStaticTriSurf( surf->geometry );
			surf->geometry = R_AllocStaticTriSurf();
		}
	} else {
		surf->geometry = R_AllocStaticTriSurf();
	}

	tri = surf->geometry;

	// note that some of the data is references, and should not be freed
	tri->deformedSurface = true;
	tri->tangentsCalculated = false;
	tri->facePlanesCalculated = false;

	tri->numIndexes = deformInfo->numIndexes;
	tri->indexes = deformInfo->indexes;
	tri->silIndexes = deformInfo->silIndexes;
	tri->numMirroredVerts = deformInfo->numMirroredVerts;
	tri->mirroredVerts = deformInfo->mirroredVerts;
	tri->numDupVerts = deformInfo->numDupVerts;
	tri->dupVerts = deformInfo->dupVerts;
	tri->numSilEdges = deformInfo->numSilEdges;
	tri->silEdges = deformInfo->silEdges;
	tri->dominantTris = deformInfo->dominantTris;
	tri->numVerts = deformInfo->numOutputVerts;

	if ( tri->verts == NULL ) {
		R_AllocStaticTriSurfVerts( tri, tri->numVerts );
		for ( i = 0; i < deformInfo->numSourceVerts; i++ ) {
			tri->verts[i].Clear();
			tri->verts[i].st = texCoords[i];
		}
	}

	const bool timing = r_skinTimers.GetInteger() > 0;

	// r_skinLazyPositions: skip CPU skinning, take bounds from joints, positions are filled on demand
	tri->skinPositionsPending = false;
	tri->skinFillWeights = NULL;
	tri->skinFillIndex = NULL;
	tri->numSkinFillWeights = 0;
	tri->numSkinnedSource = 0;
	bool lazy = false;
	idBounds lazyBounds;
	if ( r_skinLazyPositions.GetBool() && skinJoints != NULL && deformInfo->skinnedVerts != NULL && deformInfo->skinnedCache.IsValid()
		&& skinnedBounds.Num() > 0 && skinnedBoundsJoints == skinJoints->numJoints && skinFillWeights != NULL )
	{
		SkinScopeTimer timer( skinTimers.boundsNs, timing );
		// union of the world space boxes of the joint boxes, written out for speed (see idBounds::FromTransformedBounds)
		idVec3 mins( idMath::INFINITY, idMath::INFINITY, idMath::INFINITY ), maxs( -idMath::INFINITY, -idMath::INFINITY, -idMath::INFINITY );
		for ( int e = 0; e < skinnedBounds.Num(); e++ ) {
			const skinBoundsJoint_t &sb = skinnedBounds[e];
			const float *m = entJoints[sb.joint].ToFloatPtr();
			for ( int r = 0; r < 3; r++ ) {
				const float *row = m + r * 4;
				const float c = row[0] * sb.center.x + row[1] * sb.center.y + row[2] * sb.center.z + row[3];
				const float x = idMath::Fabs( row[0] ) * sb.extents.x + idMath::Fabs( row[1] ) * sb.extents.y + idMath::Fabs( row[2] ) * sb.extents.z;
				if ( c - x < mins[r] ) mins[r] = c - x;
				if ( c + x > maxs[r] ) maxs[r] = c + x;
			}
		}
		lazyBounds[0] = mins;
		lazyBounds[1] = maxs;
		lazy = !lazyBounds.IsCleared();
	}

	if ( lazy ) {
		// mirrored vertices get their texture coordinates now, positions later
		base = deformInfo->numOutputVerts - deformInfo->numMirroredVerts;
		for ( i = 0; i < deformInfo->numMirroredVerts; i++ ) {
			tri->verts[base + i] = tri->verts[deformInfo->mirroredVerts[i]];
		}
		lazyBounds.ExpandSelf( 1.0f );		// weights may not sum to exactly one
		tri->bounds = lazyBounds;
		tri->skinPositionsPending = true;
		tri->skinFillWeights = skinFillWeights;
		tri->skinFillIndex = skinFillIndex;
		tri->numSkinFillWeights = numSkinFillWeights;
		tri->numSkinnedSource = deformInfo->numSourceVerts;
	} else {
		{
			SkinScopeTimer timer( skinTimers.positionsNs, timing );
			if ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f ) {
				TransformScaledVerts( tri->verts, entJoints, ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] );
			} else {
				TransformVerts( tri->verts, entJoints );
			}

			// replicate the mirror seam vertexes
			base = deformInfo->numOutputVerts - deformInfo->numMirroredVerts;
			for ( i = 0; i < deformInfo->numMirroredVerts; i++ ) {
				tri->verts[base + i] = tri->verts[deformInfo->mirroredVerts[i]];
			}
		}

		{
			SkinScopeTimer timer( skinTimers.boundsNs, timing );
			R_BoundTriSurf( tri );
		}
	}

	// GPU skinning: the drawn vertices are computed by GPU (see GpuSkinning.h) when the ambient cache is created.
	// CPU positions above are still needed for bounds and stencil shadows, but normals and tangents are not.
	tri->skinJoints = NULL;
	tri->skinnedSource = NO_CACHE;
	tri->skinnedNoTangents = false;
	if ( skinJoints != NULL && deformInfo->skinnedVerts != NULL && deformInfo->skinnedCache.IsValid() ) {
		tri->skinJoints = skinJoints;
		tri->skinnedSource = deformInfo->skinnedCache;
		if ( timing ) {
			skinTimers.gpuSurfaces++;
			skinTimers.gpuVerts += tri->numVerts;
		}
		return;
	}
	if ( timing ) {
		skinTimers.cpuSurfaces++;
		skinTimers.cpuVerts += tri->numVerts;
		if ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f )
			skinTimers.whyScale++;
		else if ( skinJoints == NULL )
			skinTimers.whyModel++;
		else if ( deformInfo->dominantTris != NULL )
			skinTimers.whyDominant++;
		else
			skinTimers.whyNoMesh++;
	}

	// If a surface is going to be have a lighting interaction generated, it will also have to call
	// R_DeriveTangents() to get normals, tangents, and face planes.  If it only
	// needs shadows generated, it will only have to generate face planes.  If it only
	// has ambient drawing, or is culled, no additional work will be necessary
	if ( !r_useDeferredTangents.GetBool() ) {
		// set face planes, vertex normals, tangents
		SkinScopeTimer timer( skinTimers.tangentsNs, timing );
		R_DeriveTangents( tri );
	}
}

/*
====================
R_FillSkinnedPositions

Skins verts[].xyz of a surface with deferred positions (r_skinLazyPositions), like GPU does,
with the same SIMD code as normal CPU skinning.
====================
*/
void R_FillSkinnedPositions( srfTriangles_t *tri ) {
	if ( !tri->skinPositionsPending ) {
		return;
	}
	tri->skinPositionsPending = false;
	SkinScopeTimer timer( skinTimers.positionsNs, r_skinTimers.GetInteger() > 0 );	// counted together with CPU skinning
	const skinJointSet_t *sj = tri->skinJoints;
	if ( sj == NULL || tri->skinFillWeights == NULL || tri->verts == NULL ) {
		return;
	}
	// position = sum( weight_k * joint_k * weightPos_k ): the same as MD5 skinning but with skinning matrices
	SIMDProcessor->TransformVerts( tri->verts, tri->numSkinnedSource, sj->mats, tri->skinFillWeights, tri->skinFillIndex, tri->numSkinFillWeights );
	// mirror seam vertexes
	const int base = tri->numVerts - tri->numMirroredVerts;
	for ( int i = 0; i < tri->numMirroredVerts; i++ ) {
		tri->verts[base + i].xyz = tri->verts[tri->mirroredVerts[i]].xyz;
	}
}

/*
====================
idMD5Mesh::CalcBounds
====================
*/
idBounds idMD5Mesh::CalcBounds( const idJointMat *entJoints ) {
	idBounds	bounds;
	idDrawVert *verts = (idDrawVert *) _alloca16( texCoords.Num() * sizeof( idDrawVert ) );

	TransformVerts( verts, entJoints );

	SIMDProcessor->MinMax( bounds[0], bounds[1], verts, texCoords.Num() );

	return bounds;
}

/*
====================
idMD5Mesh::NearestJoint
====================
*/
int idMD5Mesh::NearestJoint( int a, int b, int c ) const {
	int i, bestJoint, vertNum, weightVertNum;
	float bestWeight;

	// duplicated vertices might not have weights
	if ( a >= 0 && a < texCoords.Num() ) {
		vertNum = a;
	} else if ( b >= 0 && b < texCoords.Num() ) {
		vertNum = b;
	} else if ( c >= 0 && c < texCoords.Num() ) {
		vertNum = c;
	} else {
		// all vertices are duplicates which shouldn't happen
		return 0;
	}

	// find the first weight for this vertex
 	weightVertNum = 0;
	for( i = 0; weightVertNum < vertNum; i++ ) {
		weightVertNum += weightIndex[i*2+1];
	}

	// get the joint for the largest weight
	bestWeight = scaledWeights[i].w;
	bestJoint = weightIndex[i*2+0] / sizeof( idJointMat );
	for( ; weightIndex[i*2+1] == 0; i++ ) {
		if ( scaledWeights[i].w > bestWeight ) {
			bestWeight = scaledWeights[i].w;
			bestJoint = weightIndex[i*2+0] / sizeof( idJointMat );
		}
	}
	return bestJoint;
}

/*
====================
idMD5Mesh::NumVerts
====================
*/
int idMD5Mesh::NumVerts( void ) const {
	return texCoords.Num();
}

/*
====================
idMD5Mesh::NumTris
====================
*/
int	idMD5Mesh::NumTris( void ) const {
	return numTris;
}

/*
====================
idMD5Mesh::NumWeights
====================
*/
int	idMD5Mesh::NumWeights( void ) const {
	return numWeights;
}

/*
====================
idMD5Mesh::UpdateVertex
====================
*/
idVec3 idMD5Mesh::UpdateVertex( const struct renderEntity_s *ent, const idJointMat *joints, int v ) const {
	// note: closely follows idMD5Mesh::UpdateSurface

	float scale = 1.0f;
	if ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f ) {
		scale = ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ];
	}

	// remap from mirror seam vertexes to original vertexes
	int base = deformInfo->numOutputVerts - deformInfo->numMirroredVerts;
	if ( v >= base )
		v = deformInfo->mirroredVerts[v - base];

	int start = vertexStarts[v];
	int end = vertexStarts[v + 1];
	assert(weightIndex[2 * end - 1] == 1);

	idDrawVert res;
	SIMDProcessor->TransformVerts( &res, 1, joints, scaledWeights + start, weightIndex + 2 * start, end - start );

	return res.xyz * scale;
}

/***********************************************************************

	idRenderModelMD5

***********************************************************************/

/*
====================
idRenderModelMD5::ParseJoint
====================
*/
void idRenderModelMD5::ParseJoint( idLexer &parser, idMD5Joint *joint, idJointQuat *defaultPose ) {
	idToken	token;
	int		num;

	//
	// parse name
	//
	parser.ReadToken( &token );
	joint->name = token;

	//
	// parse parent
	//
	num = parser.ParseInt();
	if ( num < 0 ) {
		joint->parent = NULL;
	} else {
		if ( num >= joints.Num() - 1 ) {
			parser.Error( "Invalid parent for joint '%s'", joint->name.c_str() );
		}
		joint->parent = &joints[ num ];
	}

	//
	// parse default pose
	//
	parser.Parse1DMatrix( 3, defaultPose->t.ToFloatPtr() );
	parser.Parse1DMatrix( 3, defaultPose->q.ToFloatPtr() );
	defaultPose->q.w = defaultPose->q.CalcW();
}

/*
====================
idRenderModelMD5::InitFromFile
====================
*/
void idRenderModelMD5::InitFromFile( const char *fileName ) {
	name = fileName;
	LoadModel();
}

/*
====================
idRenderModelMD5::LoadModel

used for initial loads, reloadModel, and reloading the data of purged models
Upon exit, the model will absolutely be valid, but possibly as a default model
====================
*/
void idRenderModelMD5::LoadModel() {
	int			version;
	int			i;
	int			num;
	int			parentNum;
	idToken		token;
	idLexer		parser( LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS );
	idJointQuat	*pose;
	idMD5Joint	*joint;
	idJointMat *poseMat3;

	if ( !purged ) {
		PurgeModel();
	}
	purged = false;

	if ( !parser.LoadFile( name ) ) {
		MakeDefaultModel();
		return;
	}

	parser.ExpectTokenString( MD5_VERSION_STRING );
	version = parser.ParseInt();

	if ( version != MD5_VERSION ) {
		parser.Error( "Invalid version %d.  Should be version %d\n", version, MD5_VERSION );
	}

	//
	// skip commandline
	//
	parser.ExpectTokenString( "commandline" );
	parser.ReadToken( &token );

	// parse num joints
	parser.ExpectTokenString( "numJoints" );
	num  = parser.ParseInt();
	joints.SetGranularity( 1 );
	joints.SetNum( num );
	defaultPose.SetGranularity( 1 );
	defaultPose.SetNum( num );
	poseMat3 = ( idJointMat * )_alloca16( num * sizeof( *poseMat3 ) );

	// parse num meshes
	parser.ExpectTokenString( "numMeshes" );
	num = parser.ParseInt();
	if ( num < 0 ) {
		parser.Error( "Invalid size: %d", num );
	}
	meshes.SetGranularity( 1 );
	meshes.SetNum( num );

	//
	// parse joints
	//
	parser.ExpectTokenString( "joints" );
	parser.ExpectTokenString( "{" );
	pose = defaultPose.Ptr();
	joint = joints.Ptr();
	for( i = 0; i < joints.Num(); i++, joint++, pose++ ) {
		ParseJoint( parser, joint, pose );
		poseMat3[ i ].SetRotation( pose->q.ToMat3() );
		poseMat3[ i ].SetTranslation( pose->t );
		if ( joint->parent ) {
			parentNum = joint->parent - joints.Ptr();
			pose->q = ( poseMat3[ i ].ToMat3() * poseMat3[ parentNum ].ToMat3().Transpose() ).ToQuat();
			pose->t = ( poseMat3[ i ].ToVec3() - poseMat3[ parentNum ].ToVec3() ) * poseMat3[ parentNum ].ToMat3().Transpose();
		}
	}
	parser.ExpectTokenString( "}" );

	// GPU skinning: inverse of the default pose of every joint
	// (idJointMat transforms v as v*M + t, so the inverse is M^T and -t*M^T)
	invertedDefaultPose.SetNum( joints.Num() );
	for ( i = 0; i < joints.Num(); i++ ) {
		const idMat3 rot = poseMat3[ i ].ToMat3();
		const idVec3 trans = poseMat3[ i ].ToVec3();
		invertedDefaultPose[ i ].SetRotation( rot.Transpose() );
		invertedDefaultPose[ i ].SetTranslation( -( trans * rot.Transpose() ) );
	}

	// stgatilov #6099: initialize per-joint bounds
	jointBounds.SetNum( joints.Num() );
	for( i = 0; i < jointBounds.Num(); i++ )
		jointBounds[i].Clear();

	for( i = 0; i < meshes.Num(); i++ ) {
		parser.ExpectTokenString( "mesh" );
		meshes[ i ].ParseMesh( parser, defaultPose.Num(), poseMat3, jointBounds.Ptr() );
	}

	//
	// calculate the bounds of the model
	//
	CalculateBounds( poseMat3 );

	// set the timestamp for reloadmodels
	fileSystem->ReadFile( name, NULL, &timeStamp );
}

/*
==============
idRenderModelMD5::Print
==============
*/
void idRenderModelMD5::Print() const {
	const idMD5Mesh	*mesh;
	int			i;

	common->Printf( "%s\n", name.c_str() );
	common->Printf( "Dynamic model.\n" );
	common->Printf( "Generated smooth normals.\n" );
	common->Printf( "    verts  tris weights material\n" );
	int	totalVerts = 0;
	int	totalTris = 0;
	int	totalWeights = 0;
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		totalVerts += mesh->NumVerts();
		totalTris += mesh->NumTris();
		totalWeights += mesh->NumWeights();
		common->Printf( "%2i: %5i %5i %7i %s\n", i, mesh->NumVerts(), mesh->NumTris(), mesh->NumWeights(), mesh->shader->GetName() );
	}	
	common->Printf( "-----\n" );
	common->Printf( "%4i verts.\n", totalVerts );
	common->Printf( "%4i tris.\n", totalTris );
	common->Printf( "%4i weights.\n", totalWeights );
	common->Printf( "%4i joints.\n", joints.Num() );
}

/*
==============
idRenderModelMD5::List
==============
*/
void idRenderModelMD5::List() const {
	int			i;
	const idMD5Mesh	*mesh;
	int			totalTris = 0;
	int			totalVerts = 0;

	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		totalTris += mesh->numTris;
		totalVerts += mesh->NumVerts();
	}
	common->Printf( " %4ik %3i %4i %4i %s(MD5)", Memory()/1024, meshes.Num(), totalVerts, totalTris, Name() );

	if ( defaulted ) {
		common->Printf( " (DEFAULTED)" );
	}

	common->Printf( "\n" );
}

/*
====================
idRenderModelMD5::CalculateBounds
====================
*/
void idRenderModelMD5::CalculateBounds( const idJointMat *entJoints ) {
	int			i;
	idMD5Mesh	*mesh;

	bounds.Clear();
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		bounds.AddBounds( mesh->CalcBounds( entJoints ) );
	}
}

/*
====================
idRenderModelMD5::Bounds

This calculates a rough bounds by using the joint radii without
transforming all the points
====================
*/
idBounds idRenderModelMD5::Bounds( const renderEntity_t *ent ) const {
#if 0
	// we can't calculate a rational bounds without an entity,
	// because joints could be positioned to deform it into an
	// arbitrarily large shape
	if ( !ent ) {
		common->Error( "idRenderModelMD5::Bounds: called without entity" );
	}
#endif

	if ( !ent ) {
		// this is the bounds for the reference pose
		return bounds;
	}

	return ent->bounds;
}

/*
====================
idRenderModelMD5::JointBounds

Returns per-joint local bounds (#6099)
====================
*/
const idBounds *idRenderModelMD5::JointBounds() const {
	return jointBounds.Ptr();
}

/*
====================
idRenderModelMD5::DrawJoints
====================
*/
void idRenderModelMD5::DrawJoints( const renderEntity_t *ent, const struct viewDef_s *view ) const {
	int					i;
	int					num;
	idVec3				pos;
	const idJointMat	*joint;
	const idMD5Joint	*md5Joint;
	int					parentNum;

	num = ent->numJoints;
	joint = ent->joints;
	md5Joint = joints.Ptr();	
	for( i = 0; i < num; i++, joint++, md5Joint++ ) {
		pos = ent->origin + joint->ToVec3() * ent->axis;
		if ( md5Joint->parent ) {
			parentNum = md5Joint->parent - joints.Ptr();
			session->rw->DebugLine( colorWhite, ent->origin + ent->joints[ parentNum ].ToVec3() * ent->axis, pos );
		}

		session->rw->DebugLine( colorRed,	pos, pos + joint->ToMat3()[ 0 ] * 2.0f * ent->axis );
		session->rw->DebugLine( colorGreen,	pos, pos + joint->ToMat3()[ 1 ] * 2.0f * ent->axis );
		session->rw->DebugLine( colorBlue,	pos, pos + joint->ToMat3()[ 2 ] * 2.0f * ent->axis );
	}

	idBounds bounds;

	bounds.FromTransformedBounds( ent->bounds, vec3_zero, ent->axis );
	session->rw->DebugBounds( colorMagenta, bounds, ent->origin );

	if ( ( r_jointNameScale.GetFloat() != 0.0f ) && ( bounds.Expand( 128.0f ).ContainsPoint( view->renderView.vieworg - ent->origin ) ) ) {
		idVec3	offset( 0, 0, r_jointNameOffset.GetFloat() );
		float	scale;

		scale = r_jointNameScale.GetFloat();
		joint = ent->joints;
		num = ent->numJoints;
		for( i = 0; i < num; i++, joint++ ) {
			pos = ent->origin + joint->ToVec3() * ent->axis;
			session->rw->DebugText( joints[ i ].name, pos + offset, scale, colorWhite, view->renderView.viewaxis, 1 );
		}
	}
}

/*
====================
idRenderModelMD5::UpdateSkinJoints

Computes (current joint * inverse default pose) for every joint of the instance.
Removes the joint set if GPU skinning cannot be used (cvar off, scaled skin, too many joints).
====================
*/
void idRenderModelMD5::UpdateSkinJoints( idRenderModelStatic *staticModel, const idJointMat *entJoints, bool skinScaled ) const {
	const int num = joints.Num();
	if ( !r_useGPUSkinning.GetBool() || skinScaled || num > MAX_GPU_SKINNING_JOINTS || invertedDefaultPose.Num() != num ) {
		if ( staticModel->skinJoints ) {
			Mem_Free16( staticModel->skinJoints->mats );
			delete staticModel->skinJoints;
			staticModel->skinJoints = NULL;
		}
		return;
	}
	skinJointSet_t *set = staticModel->skinJoints;
	if ( set == NULL || set->numJoints != num ) {
		if ( set ) {
			Mem_Free16( set->mats );
			delete set;
		}
		set = new skinJointSet_t;
		set->numJoints = num;
		set->mats = (idJointMat *) Mem_Alloc16( num * sizeof( idJointMat ) );
		staticModel->skinJoints = set;
	}
	// joints are uploaded again each frame this model instance is drawn
	set->frame = -1;
	set->jointBase = 0;
	for ( int j = 0; j < num; j++ ) {
		// A *= B means "apply A first, then B", so this is: undo default pose, then apply current pose
		set->mats[j] = invertedDefaultPose[j];
		set->mats[j] *= entJoints[j];
	}
}

/*
====================
idRenderModelMD5::InstantiateDynamicModel
====================
*/
idRenderModel *idRenderModelMD5::InstantiateDynamicModel( const struct renderEntity_s *ent, const struct viewDef_s *view, idRenderModel *cachedModel ) {
	TRACE_CPU_SCOPE_TEXT( "InstantiateDynamicModel", ent ? GetTraceLabel(*ent) : "[null]" )

	int					i, surfaceNum;
	idMD5Mesh			*mesh;
	idRenderModelStatic	*staticModel;

	if ( cachedModel && !r_useCachedDynamicModels.GetBool() ) {
		delete cachedModel;
		cachedModel = NULL;
	}

	if ( purged ) {
		common->DWarning( "model %s instantiated while purged", Name() );
		LoadModel();
	}

	if ( !ent->joints ) {
		common->Printf( "idRenderModelMD5::InstantiateDynamicModel: NULL joints on renderEntity for '%s'\n", Name() );
		delete cachedModel;
		return NULL;
	} else if ( ent->numJoints != joints.Num() ) {
		common->Printf( "idRenderModelMD5::InstantiateDynamicModel: renderEntity has different number of joints than model for '%s'\n", Name() );
		delete cachedModel;
		return NULL;
	}

	tr.pc.c_generateMd5++;

	if ( cachedModel ) {
		assert( dynamic_cast<idRenderModelStatic *>(cachedModel) != NULL );
		assert( idStr::Icmp( cachedModel->Name(), MD5_SnapshotName ) == 0 );
		staticModel = static_cast<idRenderModelStatic *>(cachedModel);
	} else {
		staticModel = new idRenderModelStatic;
		staticModel->InitEmpty( MD5_SnapshotName );
	}

	staticModel->bounds.Clear();

	if ( r_showSkel.GetInteger() ) {
		if ( ( view != NULL ) && ( !r_skipSuppress.GetBool() || !ent->suppressSurfaceInViewID || ( ent->suppressSurfaceInViewID != view->renderView.viewID ) ) ) {
			// only draw the skeleton
			DrawJoints( ent, view );
		}

		if ( r_showSkel.GetInteger() > 1 ) {
			// turn off the model when showing the skeleton
			staticModel->InitEmpty( MD5_SnapshotName );
			return staticModel;
		}
	}

	// GPU skinning: joint matrices shared by all surfaces of this instance
	{
		SkinScopeTimer timer( skinTimers.jointsNs, r_skinTimers.GetInteger() > 0 );
		UpdateSkinJoints( staticModel, ent->joints, ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f );
	}

	// create all the surfaces
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		// avoid deforming the surface if it will be a nodraw due to a skin remapping
		// FIXME: may have to still deform clipping hulls
		const idMaterial *shader = mesh->shader;
		
		shader = R_RemapShaderBySkin( shader, ent->customSkin, ent->customShader );
		
		if ( !shader || ( !shader->IsDrawn() && !shader->SurfaceCastsShadow() ) ) {
			staticModel->DeleteSurfaceWithId( i );
			mesh->surfaceNum = -1;
			continue;
		}

		modelSurface_t *surf;

		if ( staticModel->FindSurfaceWithId( i, surfaceNum ) ) {
			mesh->surfaceNum = surfaceNum;
			surf = &staticModel->surfaces[surfaceNum];
		} else {

			// Remove Overlays before adding new surfaces
			idOverlayOnRenderModel::RemoveOverlaySurfacesFromModel( staticModel );

			mesh->surfaceNum = staticModel->NumSurfaces();
			surf = &staticModel->surfaces.Alloc();
			surf->geometry = NULL;
			surf->material = NULL;
			surf->id = i;
		}

		mesh->UpdateSurface( ent, ent->joints, surf, staticModel->skinJoints );

		staticModel->bounds.AddPoint( surf->geometry->bounds[0] );
		staticModel->bounds.AddPoint( surf->geometry->bounds[1] );
	}

	return staticModel;
}

/*
====================
idRenderModelMD5::IsDynamicModel
====================
*/
dynamicModel_t idRenderModelMD5::IsDynamicModel() const {
	return DM_CACHED;
}

/*
====================
idRenderModelMD5::NumJoints
====================
*/
int idRenderModelMD5::NumJoints( void ) const {
	return joints.Num();
}

/*
====================
idRenderModelMD5::GetJoints
====================
*/
const idMD5Joint *idRenderModelMD5::GetJoints( void ) const {
	return joints.Ptr();
}

/*
====================
idRenderModelMD5::GetDefaultPose
====================
*/
const idJointQuat *idRenderModelMD5::GetDefaultPose( void ) const {
	return defaultPose.Ptr();
}

/*
====================
idRenderModelMD5::GetJointHandle
====================
*/
jointHandle_t idRenderModelMD5::GetJointHandle( const char *name ) const {
	const idMD5Joint *joint;
	int	i;
	
	joint = joints.Ptr();
	for( i = 0; i < joints.Num(); i++, joint++ ) {
		if ( idStr::Icmp( joint->name.c_str(), name ) == 0 ) {
			return ( jointHandle_t )i;
		}
	}

	return INVALID_JOINT;
}

/*
=====================
idRenderModelMD5::GetJointName
=====================
*/
const char *idRenderModelMD5::GetJointName( jointHandle_t handle ) const {
	if ( ( handle < 0 ) || ( handle >= joints.Num() ) ) {
		return "<invalid joint>";
	}

	return joints[ handle ].name;
}

/*
====================
idRenderModelMD5::NearestJoint
====================
*/
int idRenderModelMD5::NearestJoint( int surfaceNum, int a, int b, int c ) const {
	int i;
	const idMD5Mesh *mesh;

	if ( surfaceNum > meshes.Num() ) {
		common->Error( "idRenderModelMD5::NearestJoint: surfaceNum > meshes.Num()" );
	}

	for ( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		if ( mesh->surfaceNum == surfaceNum ) {
			return mesh->NearestJoint( a, b, c );
		}
	}
	return 0;
}

/*
====================
idRenderModelMD5::GenerateSamples
====================
*/
void idRenderModelMD5::GenerateSamples( idList<samplePointOnModel_t> &samples, const modelSamplingParameters_t &params, idRandom &rnd ) const {
	// convert T-pose: joint quaternions to joint matrices
	int jn = defaultPose.Num();

	idJointMat *jointMats = (idJointMat*)Mem_Alloc16( jn * sizeof(idJointMat) );
	SIMDProcessor->ConvertJointQuatsToJointMats( jointMats, defaultPose.Ptr(), jn );

	// fake render entity: only a few properties will be read from it
	renderEntity_t rent;
	memset( &rent, 0, sizeof(rent) );
	rent.joints = jointMats;
	rent.numJoints = defaultPose.Num();
	// generate static model in T-pose
	idRenderModel *model = const_cast<idRenderModelMD5*>(this)->InstantiateDynamicModel( &rent, nullptr, nullptr );
	assert( dynamic_cast<idRenderModelStatic*>( model ) );

	Mem_Free16(jointMats);

	// sample static surface
	model->GenerateSamples( samples, params, rnd );

	// InstantiateDynamicModel drops surfaces which don't render and don't case shadows (like e.g. nodrawsolid)
	// it seems right now that we never need them, i.e. mapper will never skin-remap them to drawable material
	// however, we need to renumber surfaces accordingly
	for ( int s = 0; s < samples.Num(); s++ ) {
		samplePointOnModel_t &smp = samples[s];
		int genSurfIdx = smp.surfaceIndex;
		const modelSurface_t *surf = model->Surface( genSurfIdx );
		assert( surf->id >= 0 && surf->id < meshes.Num() );
		smp.surfaceIndex = surf->id;
	}

	delete model;
}

/*
====================
idRenderModelMD5::GetSamplePosition
====================
*/
idVec3 idRenderModelMD5::GetSamplePosition( const struct renderEntity_s *ent, const samplePointOnModel_t &sample ) const {
	int m = sample.surfaceIndex;
	int t = sample.triangleIndex;
	int v0 = meshes[m].deformInfo->indexes[3 * t + 0];
	int v1 = meshes[m].deformInfo->indexes[3 * t + 1];
	int v2 = meshes[m].deformInfo->indexes[3 * t + 2];
	idVec3 pos0 = meshes[m].UpdateVertex( ent, ent->joints, v0 );
	idVec3 pos1 = meshes[m].UpdateVertex( ent, ent->joints, v1 );
	idVec3 pos2 = meshes[m].UpdateVertex( ent, ent->joints, v2 );
	idVec3 pos = sample.baryCoords.x * pos0 + sample.baryCoords.y * pos1 + sample.baryCoords.z * pos2;
	return pos;
}

/*
====================
idRenderModelMD5::GetSampleMaterial
====================
*/
const idMaterial *idRenderModelMD5::GetSampleMaterial( const struct renderEntity_s *ent, const samplePointOnModel_t &sample ) const {
	const idMaterial *material = meshes[sample.surfaceIndex].shader;
	material = R_RemapShaderBySkin( material, ent->customSkin, ent->customShader );
	return material;
}

/*
====================
idRenderModelMD5::TouchData

models that are already loaded at level start time
will still touch their materials to make sure they
are kept loaded
====================
*/
void idRenderModelMD5::TouchData() {
	idMD5Mesh	*mesh;
	int			i;

	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		declManager->FindMaterial( mesh->shader->GetName() );
	}
}

/*
===================
idRenderModelMD5::PurgeModel

frees all the data, but leaves the class around for dangling references,
which can regenerate the data with LoadModel()
===================
*/
void idRenderModelMD5::PurgeModel() {
	purged = true;
	joints.ClearFree();
	defaultPose.ClearFree();
	meshes.ClearFree();
}

/*
===================
idRenderModelMD5::Memory
===================
*/
int	idRenderModelMD5::Memory() const {
	size_t		total;

	total = sizeof( *this );
	total += joints.MemoryUsed() + defaultPose.MemoryUsed() + meshes.MemoryUsed();

	// count up strings
	for (int i = 0; i < joints.Num(); i++ ) {
		total += joints[i].name.DynamicMemoryUsed();
	}

	// count up meshes
	for (int i = 0 ; i < meshes.Num() ; i++ ) {
		const idMD5Mesh *mesh = &meshes[i];

		total += mesh->texCoords.MemoryUsed() + mesh->numWeights * ( sizeof( mesh->scaledWeights[0] ) + sizeof( mesh->weightIndex[0] ) * 2 );

		// sum up deform info
		total += sizeof( mesh->deformInfo );
		total += R_DeformInfoMemoryUsed( mesh->deformInfo );
	}
    return static_cast<int>(total);
}

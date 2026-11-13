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

#ifndef __VERTEXCACHE_H__
#define __VERTEXCACHE_H__

#include "renderer/backend/GpuBuffer.h"

// vertex cache calls should only be made by the front end

const int VERTCACHE_NUM_FRAMES = 3;

static const uint32 drawVertSize = sizeof(idDrawVert);
static const uint32 shadowCacheSize = sizeof(shadowCache_t);
static const uint32 VERTCACHE_FRAME_MASK = ( 1 << VERTCACHE_FRAMENUM_BITS ) - 1;

// 240 is the least common multiple between 16-byte alignment and the size of idDrawVert and shadowCache_t.
// The vertex cache positions need to be divisible by either size for glDrawElementsBaseVertex and similar
// functions.
const int VERTEX_CACHE_ALIGN = 240;
const int INDEX_CACHE_ALIGN = 16;
const int SHADOW_CACHE_ALIGN = 16;
// joint matrices are bound as uniform buffer ranges, so offsets must satisfy GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
// the OpenGL spec limits it to 256 bytes, and it is always a power of two, so 256 is safe everywhere
const int JOINT_CACHE_ALIGN = 256;
// uniform block binding point of the joint matrices (see GLSLProgram::SetDefaultUniformBlockBindings)
const int JOINT_UBO_BINDING = 2;
// size of the uniform block in glprogs/tdm_skinning.glsl (std140: vec4 per matrix row)
const int JOINT_BLOCK_BYTES = MAX_GPU_SKINNING_JOINTS * GPU_SKINNING_ROWS_PER_JOINT * 16;
// size of a single joint in the cache: 3x4 matrix of floats
static const uint32 jointMatSize = sizeof(idJointMat);
//#define ALIGN( x, a ) ( ( ( x ) + ((a)-1) ) - ( ( (x) + (a) - 1 ) % a ) )

enum cacheType_t {
	CACHE_VERTEX,
	CACHE_INDEX,
	CACHE_JOINT
};

struct geoBufferSet_t {
	GpuBuffer			indexBuffer;
	GpuBuffer			vertexBuffer;
	GpuBuffer			jointBuffer;		// uniform buffer with joint matrices for GPU skinning
	byte *				mappedVertexBase;
	byte *				mappedIndexBase;
	byte *				mappedJointBase;
	idSysInterlockedInteger	indexMemUsed;
	idSysInterlockedInteger	vertexMemUsed;
	idSysInterlockedInteger	jointMemUsed;
	int					allocations;	// number of index and vertex allocations combined

	geoBufferSet_t();
};

static const vertCacheHandle_t NO_CACHE = { 0, 0, 0, false };

enum attribBind_t {
	ATTRIB_REGULAR,
	ATTRIB_SHADOW,
};

class idVertexCache {
public:
	void			Init();
	void			Shutdown();

	// called when vertex programs are enabled or disabled, because
	// the cached data is no longer valid
	void			PurgeAll();

	// will be an int offset cast to a pointer of ARB_vertex_buffer_object
	void			VertexPosition( vertCacheHandle_t handle, attribBind_t attrib = attribBind_t::ATTRIB_REGULAR );
	void *			IndexPosition( vertCacheHandle_t handle );

	// if you need to draw something without an indexCache, this must be called to reset GL_ELEMENT_ARRAY_BUFFER
	void			UnbindIndex();

	// updates the counter for determining which temp space to use
	// and which blocks can be purged
	// Also prints debugging info when enabled
	void			EndFrame();

	// prepare a shadow buffer to fill the static cache during map load
	void			PrepareStaticCacheForUpload();

	// this data is only valid for one frame of rendering
	vertCacheHandle_t AllocVertex( const void * data, int bytes ) {
		return ActuallyAlloc( dynamicData, data, bytes, CACHE_VERTEX );
	}
	vertCacheHandle_t AllocIndex( const void * data, int bytes ) {
		return ActuallyAlloc( dynamicData, data, bytes, CACHE_INDEX );
	}

	// this data is valid until the next map load
	vertCacheHandle_t AllocStaticVertex( const void* data, int bytes );
	vertCacheHandle_t AllocStaticIndex( const void* data, int bytes );
	vertCacheHandle_t AllocStaticShadow( void* data, int bytes );	// receives ownership

	// GPU skinning: joint matrices (idJointMat array), valid for one frame of rendering
	vertCacheHandle_t AllocJoint( const void * data, int bytes ) {
		return ActuallyAlloc( dynamicData, data, bytes, CACHE_JOINT );
	}
	// binds the joint matrices of the given handle to the joint uniform block
	// returns false if the handle is not valid
	bool			BindJointBuffer( vertCacheHandle_t handle );

	// GPU skinning: reserves space for vertices that will be written by the GPU (transform feedback) in backend
	vertCacheHandle_t AllocVertexForGpuWrite( int bytes ) {
		return ActuallyAlloc( dynamicData, NULL, bytes, CACHE_VERTEX );
	}
	// binds the given range of dynamic vertex buffer as transform feedback output (index 0)
	bool			BindTransformFeedbackRange( vertCacheHandle_t handle );
	// copies bytes from a GL buffer (written by GPU) into the memory of the handle, which was returned by AllocVertexForGpuWrite
	bool			CopyToVertexRange( GLuint srcBuffer, GLintptr srcOffset, vertCacheHandle_t handle );

	// GL object of the dynamic vertex buffer, for reading it in shaders as texture buffer (GPU shadow volumes)
	GLuint			GetDynamicVertexBufferObject() const {
		return dynamicData.vertexBuffer.GetAPIObject();
	}

	// Returns false if it's been purged
	// This can only be called by the front end, the back end should only be looking at
	// vertCacheHandle_t that are already validated.
	bool			CacheIsCurrent( const vertCacheHandle_t handle ) const {
		return handle.isStatic || ( handle.IsValid() && handle.frameNumber == ( currentFrame & VERTCACHE_FRAME_MASK ) );
	}

	int GetBaseVertex() {
		return basePointer;
	}

public:
	static idCVar	r_showVertexCache;
	static idCVar	r_staticVertexMemory;
	static idCVar	r_staticIndexMemory;
	static idCVar	r_frameVertexMemory;
	static idCVar	r_frameIndexMemory;
	static idCVar	r_frameJointMemory;

	drawSurf_t		screenRectSurf;

private:

	int				currentFrame;			// for purgable block tracking
	int				basePointer;

	geoBufferSet_t	dynamicData;
	GLuint			staticVertexBuffer;
	GLuint			staticIndexBuffer;
	GLuint			staticShadowBuffer;

	GLuint			currentIndexBuffer;

	int				indexAllocCount, vertexAllocCount;
	int				indexUseCount, vertexUseCount;

	int				currentVertexCacheSize;
	int				currentIndexCacheSize;
	int				currentJointCacheSize;

	// Try to make room for <bytes> bytes
	vertCacheHandle_t ActuallyAlloc( geoBufferSet_t & vcs, const void * data, int bytes, cacheType_t type );
};

extern idVertexCache vertexCache;

#endif
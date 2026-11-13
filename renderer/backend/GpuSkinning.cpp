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
#include "renderer/backend/GpuSkinning.h"
#include "renderer/backend/GLSLProgram.h"
#include "renderer/backend/GLSLProgramManager.h"
#include "renderer/backend/GLSLUniforms.h"
#include "renderer/backend/VertexArrayState.h"

GpuSkinning gpuSkinning;
SkinTimers skinTimers;

// size of one vertex in the scratch buffer: two vec4 (see skin_positions.vert.glsl)
static const int SCRATCH_VERTEX_BYTES = 32;

// size of one triangle in the face buffer: three vec4 (see skin_faces.vert.glsl)
static const int FACE_BYTES = 48;

// joints of surfaces: skin_positions.vert.glsl and skin_direct.vert.glsl
struct SkinJointUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( SkinJointUniforms )

	DEFINE_UNIFORM( sampler, joints )
	DEFINE_UNIFORM( sampler, instances )
	DEFINE_UNIFORM( int, instanceBase )
};

struct SkinFacesUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( SkinFacesUniforms )

	DEFINE_UNIFORM( sampler, scratch )
	DEFINE_UNIFORM( sampler, indexes )
	DEFINE_UNIFORM( int, scratchBase )
	DEFINE_UNIFORM( int, numVerts )
};

struct SkinTangentsUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( SkinTangentsUniforms )

	DEFINE_UNIFORM( sampler, scratch )
	DEFINE_UNIFORM( sampler, vertInfo )
	DEFINE_UNIFORM( sampler, lists )
	DEFINE_UNIFORM( sampler, faces )
	DEFINE_UNIFORM( int, scratchBase )
	DEFINE_UNIFORM( int, faceBase )
	DEFINE_UNIFORM( int, numVerts )
	DEFINE_UNIFORM( int, numTris )
};

// GPU shadow volumes: skin_shadow.vert.glsl
struct SkinShadowUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( SkinShadowUniforms )

	DEFINE_UNIFORM( mat4, projectionMatrix )
	DEFINE_UNIFORM( mat4, modelViewMatrix )
	DEFINE_UNIFORM( vec4, localLightOrigin )
	DEFINE_UNIFORM( sampler, verts )
	DEFINE_UNIFORM( sampler, silEdges )
	DEFINE_UNIFORM( sampler, indexes )
	DEFINE_UNIFORM( int, vertBase )
	DEFINE_UNIFORM( int, numEdges )
	DEFINE_UNIFORM( int, numTris )
};
static const int SHADOW_VERT_UNIT = 6;
static const int SHADOW_EDGE_UNIT = 7;
static const int SHADOW_INDEX_UNIT = 8;

// texture units of the joint and instance texture buffers
static const int JOINT_TEXTURE_UNIT = 4;
static const int INSTANCE_TEXTURE_UNIT = 5;

vertCacheHandle_t GpuSkinning::AllocMesh( const MeshData &data ) {
	if ( data.verts == nullptr || data.numVerts <= 0 ) {
		return NO_CACHE;
	}
	std::lock_guard<std::mutex> lock( meshMutex );
	int index;
	if ( !freeMeshes.empty() ) {
		index = freeMeshes.back();
		freeMeshes.pop_back();
	} else {
		index = (int)meshes.size();
		meshes.push_back( new MeshEntry() );
	}
	MeshEntry &entry = *meshes[index];
	entry.inUse = true;
	entry.data = data;
	return vertCacheHandle_t::Create( data.numVerts * sizeof( idDrawVertSkinned ), index, vertCacheHandle_t::SKINNED_MARKER, true );
}

void GpuSkinning::FreeMesh( vertCacheHandle_t handle ) {
	if ( !handle.IsSkinnedBuffer() ) {
		return;
	}
	std::lock_guard<std::mutex> lock( meshMutex );
	const int index = handle.offset;
	if ( index < 0 || index >= (int)meshes.size() || !meshes[index]->inUse ) {
		return;
	}
	MeshEntry &entry = *meshes[index];
	if ( entry.glReady ) {
		// GL objects can be deleted only by the backend
		DeletedGl del;
		del.vbo = entry.vbo;
		memcpy( del.buffers, entry.buffers, sizeof( del.buffers ) );
		memcpy( del.textures, entry.textures, sizeof( del.textures ) );
		pendingDelete.push_back( del );
	}
	entry = MeshEntry();
	freeMeshes.push_back( index );
}

vertCacheHandle_t GpuSkinning::AddJob( const srfTriangles_t *tri, bool needsLighting ) {
	skinJointSet_t *sj = tri->skinJoints;
	if ( sj == nullptr || !tri->skinnedSource.IsSkinnedBuffer() || tri->numVerts <= 0 ) {
		return NO_CACHE;
	}

	std::lock_guard<std::mutex> lock( jobMutex );

	const int index = tri->skinnedSource.offset;
	MeshEntry *mesh;
	{
		std::lock_guard<std::mutex> lockMesh( meshMutex );
		if ( index < 0 || index >= (int)meshes.size() || !meshes[index]->inUse || meshes[index]->data.numVerts != tri->numVerts ) {
			return NO_CACHE;
		}
		mesh = meshes[index];
	}

	// joint matrices are shared by all surfaces of one model instance
	if ( sj->frame != frameCounter ) {
		const size_t numFloats = (size_t)sj->numJoints * 12;
		if ( ( ( pendingJoints.size() + numFloats ) / 12 ) * 3 > (size_t)maxJointTexels ) {
			return NO_CACHE;
		}
		sj->jointBase = (int)( pendingJoints.size() / 12 );
		pendingJoints.insert( pendingJoints.end(), (const float *)sj->mats, (const float *)sj->mats + numFloats );
		sj->frame = frameCounter;
	}

	vertCacheHandle_t dst = vertexCache.AllocVertexForGpuWrite( tri->numVerts * sizeof( idDrawVert ) );
	if ( !dst.IsValid() ) {
		return NO_CACHE;
	}

	Job job;
	job.mesh = mesh;
	job.numVerts = tri->numVerts;
	job.jointBase = sj->jointBase;
	job.dst = dst;
	job.lit = needsLighting;
	job.valid = true;
	pendingJobs.push_back( job );
	return dst;
}

bool GpuSkinning::UpgradeJob( vertCacheHandle_t handle ) {
	std::lock_guard<std::mutex> lock( jobMutex );
	// the job was added during this frame, so it is among the most recent ones
	for ( size_t i = pendingJobs.size(); i > 0; i-- ) {
		Job &job = pendingJobs[i - 1];
		if ( job.dst.offset == handle.offset && job.dst.frameNumber == handle.frameNumber ) {
			job.lit = true;
			return true;
		}
	}
	return false;
}

void GpuSkinning::EndFrame() {
	// frontend has finished the frame: its jobs will be executed by backend (which runs one frame behind)
	// if backend did not execute previous jobs, then the vertex cache memory is already reused, so drop them
	std::lock_guard<std::mutex> lock( jobMutex );
	readyJobs.swap( pendingJobs );
	pendingJobs.clear();
	readyJoints.swap( pendingJoints );
	pendingJoints.clear();
	frameCounter++;
}

void GpuSkinning::LoadPositionsProgram( GLSLProgram *program ) {
	program->AttachVertexShader( "stages/skinning/skin_positions.vert.glsl" );
	vaState.BindAttributesToProgram( program );
	const char *varyings[] = { "tf_a", "tf_b" };
	program->SetTransformFeedbackVaryings( 2, varyings );
	program->Link();
	SkinJointUniforms *uniforms = program->GetUniformGroup<SkinJointUniforms>();
	uniforms->joints.Set( JOINT_TEXTURE_UNIT );
	uniforms->instances.Set( INSTANCE_TEXTURE_UNIT );
	uniforms->instanceBase.Set( 0 );
}

void GpuSkinning::LoadFacesProgram( GLSLProgram *program ) {
	program->AttachVertexShader( "stages/skinning/skin_faces.vert.glsl" );
	vaState.BindAttributesToProgram( program );
	const char *varyings[] = { "tf_n", "tf_t0", "tf_t1" };
	program->SetTransformFeedbackVaryings( 3, varyings );
	program->Link();
	SkinFacesUniforms *uniforms = program->GetUniformGroup<SkinFacesUniforms>();
	uniforms->scratch.Set( 0 );
	uniforms->indexes.Set( 1 );
	uniforms->scratchBase.Set( 0 );
	uniforms->numVerts.Set( 0 );
}

void GpuSkinning::LoadDirectProgram( GLSLProgram *program ) {
	program->AttachVertexShader( "stages/skinning/skin_direct.vert.glsl" );
	vaState.BindAttributesToProgram( program );
	const char *varyings[] = { "tf_xyz", "tf_st", "tf_normal", "tf_tangent0", "tf_tangent1", "tf_color" };
	program->SetTransformFeedbackVaryings( 6, varyings );
	program->Link();
	SkinJointUniforms *uniforms = program->GetUniformGroup<SkinJointUniforms>();
	uniforms->joints.Set( JOINT_TEXTURE_UNIT );
	uniforms->instances.Set( INSTANCE_TEXTURE_UNIT );
	uniforms->instanceBase.Set( 0 );
}

void GpuSkinning::LoadShadowProgram( GLSLProgram *program ) {
	program->AttachVertexShader( "stages/skinning/skin_shadow.vert.glsl" );
	program->Link();
	SkinShadowUniforms *uniforms = program->GetUniformGroup<SkinShadowUniforms>();
	uniforms->verts.Set( SHADOW_VERT_UNIT );
	uniforms->silEdges.Set( SHADOW_EDGE_UNIT );
	uniforms->indexes.Set( SHADOW_INDEX_UNIT );
	uniforms->vertBase.Set( 0 );
	uniforms->numEdges.Set( 0 );
	uniforms->numTris.Set( 0 );
}

void GpuSkinning::LoadTangentsProgram( GLSLProgram *program ) {
	program->AttachVertexShader( "stages/skinning/skin_tangents.vert.glsl" );
	vaState.BindAttributesToProgram( program );
	// must match idDrawVert: xyz, st, normal, tangents[0], tangents[1], color
	const char *varyings[] = { "tf_xyz", "tf_st", "tf_normal", "tf_tangent0", "tf_tangent1", "tf_color" };
	program->SetTransformFeedbackVaryings( 6, varyings );
	program->Link();
	SkinTangentsUniforms *uniforms = program->GetUniformGroup<SkinTangentsUniforms>();
	uniforms->scratch.Set( 0 );
	uniforms->vertInfo.Set( 1 );
	uniforms->lists.Set( 2 );
	uniforms->faces.Set( 3 );
	uniforms->scratchBase.Set( 0 );
	uniforms->faceBase.Set( 0 );
	uniforms->numVerts.Set( 0 );
	uniforms->numTris.Set( 0 );
}

void GpuSkinning::Init() {
	static_assert( sizeof( idDrawVert ) == 60, "transform feedback in skin_tangents.vert.glsl writes 15 words" );
	static_assert( sizeof( idJointMat ) == 12 * sizeof( float ), "joint matrices are uploaded as 3 vec4" );
	GLint maxTexels = 0;
	qglGetIntegerv( GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels );
	{
		std::lock_guard<std::mutex> lock( jobMutex );
		maxJointTexels = std::min( maxTexels, 1 << 22 );
	}
	positionsProgram = programManager->LoadFromGenerator( "skin_positions", [this]( GLSLProgram *program ) { LoadPositionsProgram( program ); } );
	facesProgram = programManager->LoadFromGenerator( "skin_faces", [this]( GLSLProgram *program ) { LoadFacesProgram( program ); } );
	tangentsProgram = programManager->LoadFromGenerator( "skin_tangents", [this]( GLSLProgram *program ) { LoadTangentsProgram( program ); } );
	directProgram = programManager->LoadFromGenerator( "skin_direct", [this]( GLSLProgram *program ) { LoadDirectProgram( program ); } );
	shadowProgram = programManager->LoadFromGenerator( "skin_shadow", [this]( GLSLProgram *program ) { LoadShadowProgram( program ); } );
}

void GpuSkinning::DeleteGl( const DeletedGl &del ) {
	if ( del.vbo ) {
		qglDeleteBuffers( 1, &del.vbo );
	}
	qglDeleteBuffers( 4, del.buffers );
	qglDeleteTextures( 4, del.textures );
}

void GpuSkinning::DeletePending() {
	std::lock_guard<std::mutex> lock( meshMutex );
	for ( const DeletedGl &del : pendingDelete ) {
		DeleteGl( del );
	}
	pendingDelete.clear();
}

void GpuSkinning::Shutdown() {
	DeletePending();
	{
		std::lock_guard<std::mutex> lock( meshMutex );
		for ( MeshEntry *entry : meshes ) {
			if ( entry->glReady ) {
				DeletedGl del;
				del.vbo = entry->vbo;
				memcpy( del.buffers, entry->buffers, sizeof( del.buffers ) );
				memcpy( del.textures, entry->textures, sizeof( del.textures ) );
				DeleteGl( del );
				entry->vbo = 0;
				memset( entry->buffers, 0, sizeof( entry->buffers ) );
				memset( entry->textures, 0, sizeof( entry->textures ) );
				entry->glReady = false;	// buffers are created again if the mesh is used after video restart
			}
		}
	}
	if ( scratchBuffer ) {
		qglDeleteBuffers( 1, &scratchBuffer );
		qglDeleteTextures( 1, &scratchTexture );
		scratchBuffer = scratchTexture = 0;
		scratchCapacity = 0;
	}
	if ( faceBuffer ) {
		qglDeleteBuffers( 1, &faceBuffer );
		qglDeleteTextures( 1, &faceTexture );
		faceBuffer = faceTexture = 0;
		faceCapacity = 0;
	}
	if ( shadowVertTexture ) {
		qglDeleteTextures( 1, &shadowVertTexture );
		shadowVertTexture = 0;
	}
	if ( stagingBuffer ) {
		qglDeleteBuffers( 1, &stagingBuffer );
		stagingBuffer = 0;
		stagingCapacity = 0;
	}
	if ( jointBuffer ) {
		qglDeleteBuffers( 1, &jointBuffer );
		qglDeleteTextures( 1, &jointTexture );
		jointBuffer = jointTexture = 0;
	}
	if ( instanceBuffer ) {
		qglDeleteBuffers( 1, &instanceBuffer );
		qglDeleteTextures( 1, &instanceTexture );
		instanceBuffer = instanceTexture = 0;
	}
	{
		std::lock_guard<std::mutex> lock( jobMutex );
		pendingJobs.clear();
		readyJobs.clear();
		pendingJoints.clear();
		readyJoints.clear();
		frameCounter++;		// joints queued before are lost
	}
}

static GLuint CreateStaticBuffer( const void *data, GLsizeiptr bytes ) {
	GLuint buffer = 0;
	qglGenBuffers( 1, &buffer );
	// bind to a target which does not disturb the state tracked by vertex array state
	qglBindBuffer( GL_COPY_WRITE_BUFFER, buffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, bytes, data, GL_STATIC_DRAW );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	return buffer;
}

static GLuint CreateBufferTexture( GLenum format, GLuint buffer ) {
	GLuint texture = 0;
	qglGenTextures( 1, &texture );
	// texture buffers are a separate binding target, so this does not interfere with 2D textures of the engine
	qglActiveTexture( GL_TEXTURE0 );
	qglBindTexture( GL_TEXTURE_BUFFER, texture );
	qglTexBuffer( GL_TEXTURE_BUFFER, format, buffer );
	return texture;
}

void GpuSkinning::EnsureGl( MeshEntry &mesh ) {
	if ( mesh.glReady ) {
		return;
	}
	const MeshData &d = mesh.data;
	mesh.vbo = CreateStaticBuffer( d.verts, (GLsizeiptr)d.numVerts * sizeof( idDrawVertSkinned ) );
	GL_SetDebugLabel( GL_BUFFER, mesh.vbo, "SkinnedMeshVertices" );

	mesh.buffers[0] = CreateStaticBuffer( d.vertInfo, (GLsizeiptr)d.numVerts * 4 * sizeof( int ) );

	static const int zero[1] = { 0 };
	if ( d.numTriLists > 0 ) {
		mesh.buffers[1] = CreateStaticBuffer( d.triLists, (GLsizeiptr)d.numTriLists * sizeof( int ) );
	} else {
		mesh.buffers[1] = CreateStaticBuffer( zero, sizeof( zero ) );
	}

	if ( sizeof( glIndex_t ) == sizeof( int ) ) {
		mesh.buffers[2] = CreateStaticBuffer( d.indexes, (GLsizeiptr)d.numIndexes * sizeof( int ) );
	} else {
		std::vector<int> converted( d.indexes, d.indexes + d.numIndexes );
		mesh.buffers[2] = CreateStaticBuffer( converted.data(), converted.size() * sizeof( int ) );
	}

	mesh.textures[0] = CreateBufferTexture( GL_RGBA32I, mesh.buffers[0] );
	mesh.textures[1] = CreateBufferTexture( GL_R32I, mesh.buffers[1] );
	mesh.textures[2] = CreateBufferTexture( GL_R32I, mesh.buffers[2] );

	if ( d.numSilEdges > 0 ) {
		mesh.buffers[3] = CreateStaticBuffer( d.silEdges, (GLsizeiptr)d.numSilEdges * 4 * sizeof( int ) );
	} else {
		mesh.buffers[3] = CreateStaticBuffer( zero, sizeof( zero ) );
	}
	mesh.textures[3] = CreateBufferTexture( GL_RGBA32I, mesh.buffers[3] );
	mesh.glReady = true;
}

void GpuSkinning::EnsureScratch( int numVerts ) {
	if ( numVerts <= scratchCapacity && scratchBuffer != 0 ) {
		return;
	}
	int newCapacity = std::max( numVerts, 4096 );
	newCapacity = std::max( newCapacity, scratchCapacity * 2 );
	if ( scratchBuffer == 0 ) {
		qglGenBuffers( 1, &scratchBuffer );
		qglGenTextures( 1, &scratchTexture );
		GL_SetDebugLabel( GL_BUFFER, scratchBuffer, "SkinningScratch" );
	}
	qglBindBuffer( GL_COPY_WRITE_BUFFER, scratchBuffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, (GLsizeiptr)newCapacity * SCRATCH_VERTEX_BYTES, nullptr, GL_DYNAMIC_COPY );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	qglActiveTexture( GL_TEXTURE0 );
	qglBindTexture( GL_TEXTURE_BUFFER, scratchTexture );
	qglTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, scratchBuffer );
	scratchCapacity = newCapacity;
}

void GpuSkinning::EnsureFaces( int numTris ) {
	if ( numTris <= faceCapacity && faceBuffer != 0 ) {
		return;
	}
	int newCapacity = std::max( numTris, 4096 );
	newCapacity = std::max( newCapacity, faceCapacity * 2 );
	if ( faceBuffer == 0 ) {
		qglGenBuffers( 1, &faceBuffer );
		qglGenTextures( 1, &faceTexture );
		GL_SetDebugLabel( GL_BUFFER, faceBuffer, "SkinningFaces" );
	}
	qglBindBuffer( GL_COPY_WRITE_BUFFER, faceBuffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, (GLsizeiptr)newCapacity * FACE_BYTES, nullptr, GL_DYNAMIC_COPY );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	qglActiveTexture( GL_TEXTURE0 );
	qglBindTexture( GL_TEXTURE_BUFFER, faceTexture );
	qglTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, faceBuffer );
	faceCapacity = newCapacity;
}

void GpuSkinning::EnsureStaging( int numVerts ) {
	if ( numVerts <= stagingCapacity && stagingBuffer != 0 ) {
		return;
	}
	int newCapacity = std::max( numVerts, 4096 );
	newCapacity = std::max( newCapacity, stagingCapacity * 2 );
	if ( stagingBuffer == 0 ) {
		qglGenBuffers( 1, &stagingBuffer );
		GL_SetDebugLabel( GL_BUFFER, stagingBuffer, "SkinningStaging" );
	}
	qglBindBuffer( GL_COPY_WRITE_BUFFER, stagingBuffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, (GLsizeiptr)newCapacity * sizeof( idDrawVert ), nullptr, GL_DYNAMIC_COPY );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	stagingCapacity = newCapacity;
}

// replaces contents of a texture buffer with new data (data of every frame is orphaned)
void GpuSkinning::UploadTexelBuffer( GLuint &buffer, GLuint &texture, GLenum format, const void *data, GLsizeiptr bytes ) {
	if ( buffer == 0 ) {
		qglGenBuffers( 1, &buffer );
		qglGenTextures( 1, &texture );
	}
	qglBindBuffer( GL_COPY_WRITE_BUFFER, buffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, bytes, data, GL_STREAM_DRAW );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	qglBindTexture( GL_TEXTURE_BUFFER, texture );
	qglTexBuffer( GL_TEXTURE_BUFFER, format, buffer );
}

void GpuSkinning::ReportTimers() {
	const int period = r_skinTimers.GetInteger();
	if ( period <= 0 || timerCalls < period ) {
		return;
	}
	const double n = timerCalls;
	auto us = [&]( std::atomic<long long> &c ) { return (double)c.exchange( 0 ) / 1000.0 / n; };
	const double positions = us( skinTimers.positionsNs ), bounds = us( skinTimers.boundsNs ), tangents = us( skinTimers.tangentsNs );
	const double joints = us( skinTimers.jointsNs ), ambient = us( skinTimers.ambientNs );
	const double facing = us( skinTimers.facingNs ), cullBits = us( skinTimers.cullBitsNs ), lightTris = us( skinTimers.lightTrisNs ), shadows = us( skinTimers.shadowNs );
	const long long cs = skinTimers.cpuSurfaces.exchange( 0 ), cv = skinTimers.cpuVerts.exchange( 0 );
	const long long gs = skinTimers.gpuSurfaces.exchange( 0 ), gv = skinTimers.gpuVerts.exchange( 0 );
	const long long wScale = skinTimers.whyScale.exchange( 0 ), wModel = skinTimers.whyModel.exchange( 0 );
	const long long wDominant = skinTimers.whyDominant.exchange( 0 ), wNoMesh = skinTimers.whyNoMesh.exchange( 0 );
	common->Printf( "skinTimers (avg per call, %d calls, us): frontend: positions %.0f bounds %.0f tangents %.0f joints %.0f ambientCache %.0f | "
		"surfaces: cpu %.1f (%.0f verts) gpu %.1f (%.0f verts) | backend: Execute cpu %.0f gpu %.0f, jobs %.1f in %.1f batches\n",
		timerCalls, positions, bounds, tangents, joints, ambient,
		cs / n, cv / n, gs / n, gv / n, executeCpuNs / 1000.0 / n, gpuNs / 1000.0 / n, timerJobs / n, timerBatches / n );
	common->Printf( "skinTimers: light/shadow work on deformed surfaces (avg per call, us): facing %.0f cullBits %.0f lightTris %.0f shadows %.0f\n", facing, cullBits, lightTris, shadows );
	if ( cs > 0 ) {
		common->Printf( "skinTimers: why CPU skinning (surfaces per call): skinScale %.1f, model not GPU-ready %.1f, unsmoothedTangents %.1f, no skinned mesh %.1f\n",
			wScale / n, wModel / n, wDominant / n, wNoMesh / n );
	}
	timerCalls = timerJobs = timerBatches = 0;
	gpuNs = executeCpuNs = 0;
}

void GpuSkinning::Execute() {
	DeletePending();

	const bool timing = r_skinTimers.GetInteger() > 0;
	long long startNs = 0;
	bool queryStarted = false;
	if ( timing ) {
		startNs = SkinNowNs();
		timerCalls++;
		// collect finished GPU timer queries of earlier calls
		for ( int q = 0; q < 4; q++ ) {
			if ( timerPending[q] ) {
				GLint available = 0;
				qglGetQueryObjectiv( timerQueries[q], GL_QUERY_RESULT_AVAILABLE, &available );
				if ( available ) {
					GLuint64 ns = 0;
					qglGetQueryObjectui64v( timerQueries[q], GL_QUERY_RESULT, &ns );
					gpuNs += (long long)ns;
					timerPending[q] = false;
				}
			}
		}
	}
	// reports and writes the CPU time when leaving, whatever the way
	struct Finish {
		GpuSkinning *self; bool timing; long long start; bool *queryStarted;
		~Finish() {
			if ( !timing ) return;
			if ( *queryStarted ) {
				qglEndQuery( GL_TIME_ELAPSED );
				self->timerPending[self->timerIndex] = true;
				self->timerIndex = ( self->timerIndex + 1 ) % 4;
			}
			self->executeCpuNs += SkinNowNs() - start;
			self->ReportTimers();
		}
	} finish = { this, timing, startNs, &queryStarted };

	// only the backend thread touches readyJobs
	std::vector<Job> &jobs = readyJobs;
	if ( jobs.empty() ) {
		readyJoints.clear();
		return;
	}
	TRACE_GL_SCOPE( "GpuSkinning" );
	if ( timing && !timerPending[timerIndex] ) {
		if ( timerQueries[timerIndex] == 0 ) {
			qglGenQueries( 4, timerQueries );
		}
		qglBeginQuery( GL_TIME_ELAPSED, timerQueries[timerIndex] );
		queryStarted = true;
	}

	// validate, prepare GL objects of meshes
	std::vector<int> order;
	order.reserve( jobs.size() );
	{
		std::lock_guard<std::mutex> lock( meshMutex );
		for ( size_t i = 0; i < jobs.size(); i++ ) {
			Job &job = jobs[i];
			if ( !job.mesh->inUse || job.mesh->data.numVerts != job.numVerts ) {
				job.valid = false;
				continue;
			}
			EnsureGl( *job.mesh );
			order.push_back( (int)i );
		}
	}
	if ( order.empty() || readyJoints.empty() ) {
		jobs.clear();
		readyJoints.clear();
		return;
	}

	// group surfaces of the same mesh: every group is drawn with a single instanced draw per pass
	std::stable_sort( order.begin(), order.end(), [&]( int a, int b ) {
		// unlit surfaces first: every pass writes one contiguous range of its output buffer
		if ( jobs[a].lit != jobs[b].lit )
			return jobs[a].lit < jobs[b].lit;
		return jobs[a].mesh < jobs[b].mesh;
	} );
	struct Batch {
		MeshEntry *mesh;
		bool lit;
		int first, count;		// range in order[]
		int numVerts, numTris;
		int scratchBase, faceBase;	// in vertices / triangles (lit only)
		int stagingBase;		// in vertices
	};
	std::vector<Batch> batches;
	std::vector<int> instances;		// first joint of every surface, in order[] order
	instances.reserve( order.size() );
	int totalScratch = 0, totalFaces = 0, totalStaging = 0, totalUnlit = 0;
	for ( size_t k = 0; k < order.size(); k++ ) {
		const Job &job = jobs[order[k]];
		if ( batches.empty() || batches.back().mesh != job.mesh || batches.back().lit != job.lit ) {
			Batch b;
			b.mesh = job.mesh;
			b.lit = job.lit;
			b.first = (int)k;
			b.count = 0;
			b.numVerts = job.numVerts;
			b.numTris = job.mesh->data.numIndexes / 3;
			b.scratchBase = totalScratch;
			b.faceBase = totalFaces;
			b.stagingBase = totalStaging;
			batches.push_back( b );
		}
		Batch &b = batches.back();
		b.count++;
		instances.push_back( job.jointBase );
		totalStaging += job.numVerts;
		if ( !job.lit ) {
			totalUnlit += job.numVerts;
		}
		if ( job.lit ) {
			totalScratch += job.numVerts;
			totalFaces += b.numTris;
		}
	}
	timerJobs += (int)order.size();
	timerBatches += (int)batches.size();
	if ( totalScratch > 0 ) {
		EnsureScratch( totalScratch );
		EnsureFaces( totalFaces );
	}
	EnsureStaging( totalStaging );

	const int savedTmu = backEnd.glState.currenttmu;
	qglEnable( GL_RASTERIZER_DISCARD );

	// joints and instances of the whole frame
	qglActiveTexture( GL_TEXTURE0 + JOINT_TEXTURE_UNIT );
	UploadTexelBuffer( jointBuffer, jointTexture, GL_RGBA32F, readyJoints.data(), (GLsizeiptr)readyJoints.size() * sizeof( float ) );
	qglActiveTexture( GL_TEXTURE0 + INSTANCE_TEXTURE_UNIT );
	UploadTexelBuffer( instanceBuffer, instanceTexture, GL_R32I, instances.data(), (GLsizeiptr)instances.size() * sizeof( int ) );

	// Every pass is ONE transform feedback session over the whole output range, with a draw per batch inside:
	// output of consecutive draws is appended. (Begin/EndTransformFeedback per batch was very expensive.)
	// Uniforms, textures and vertex buffers can change between draws, the program cannot.
	const GLsizeiptr stagingUnlitBytes = (GLsizeiptr)totalUnlit * sizeof( idDrawVert );
	const GLsizeiptr stagingLitBytes = (GLsizeiptr)( totalStaging - totalUnlit ) * sizeof( idDrawVert );

	// batches of surfaces which don't need lighting: skin positions, write final vertices
	if ( totalUnlit > 0 ) {
		directProgram->Activate();
		SkinJointUniforms *uniforms = directProgram->GetUniformGroup<SkinJointUniforms>();
		qglBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, stagingBuffer, 0, stagingUnlitBytes );
		qglBeginTransformFeedback( GL_POINTS );
		for ( const Batch &b : batches ) {
			if ( b.lit )
				continue;
			uniforms->instanceBase.Set( b.first );
			vaState.BindVertexBufferAndSetVertexFormat( b.mesh->vbo, VF_SKINNED );
			qglDrawArraysInstanced( GL_POINTS, 0, b.numVerts, b.count );
		}
		qglEndTransformFeedback();
	}

	if ( totalScratch > 0 ) {
		// pass 1: skin positions
		positionsProgram->Activate();
		SkinJointUniforms *jointUniforms = positionsProgram->GetUniformGroup<SkinJointUniforms>();
		qglBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, scratchBuffer, 0, (GLsizeiptr)totalScratch * SCRATCH_VERTEX_BYTES );
		qglBeginTransformFeedback( GL_POINTS );
		for ( const Batch &b : batches ) {
			if ( !b.lit )
				continue;
			jointUniforms->instanceBase.Set( b.first );
			vaState.BindVertexBufferAndSetVertexFormat( b.mesh->vbo, VF_SKINNED );
			qglDrawArraysInstanced( GL_POINTS, 0, b.numVerts, b.count );
		}
		qglEndTransformFeedback();

		// pass 2: unit normal and tangents of every triangle
		if ( totalFaces > 0 ) {
			facesProgram->Activate();
			SkinFacesUniforms *faceUniforms = facesProgram->GetUniformGroup<SkinFacesUniforms>();
			qglActiveTexture( GL_TEXTURE0 );
			qglBindTexture( GL_TEXTURE_BUFFER, scratchTexture );
			qglBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, faceBuffer, 0, (GLsizeiptr)totalFaces * FACE_BYTES );
			qglBeginTransformFeedback( GL_POINTS );
			for ( const Batch &b : batches ) {
				if ( !b.lit || b.numTris <= 0 )
					continue;
				qglActiveTexture( GL_TEXTURE1 );
				qglBindTexture( GL_TEXTURE_BUFFER, b.mesh->textures[2] );
				faceUniforms->scratchBase.Set( b.scratchBase );
				faceUniforms->numVerts.Set( b.numVerts );
				vaState.BindVertexBufferAndSetVertexFormat( b.mesh->vbo, VF_SKINNED );
				qglDrawArraysInstanced( GL_POINTS, 0, b.numTris, b.count );
			}
			qglEndTransformFeedback();
		}

		// pass 3: gather per vertex, write final vertices
		tangentsProgram->Activate();
		SkinTangentsUniforms *uniforms = tangentsProgram->GetUniformGroup<SkinTangentsUniforms>();
		qglActiveTexture( GL_TEXTURE0 );
		qglBindTexture( GL_TEXTURE_BUFFER, scratchTexture );
		qglActiveTexture( GL_TEXTURE3 );
		qglBindTexture( GL_TEXTURE_BUFFER, faceTexture );
		qglBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, stagingBuffer, stagingUnlitBytes, stagingLitBytes );
		qglBeginTransformFeedback( GL_POINTS );
		for ( const Batch &b : batches ) {
			if ( !b.lit )
				continue;
			qglActiveTexture( GL_TEXTURE1 );
			qglBindTexture( GL_TEXTURE_BUFFER, b.mesh->textures[0] );
			qglActiveTexture( GL_TEXTURE2 );
			qglBindTexture( GL_TEXTURE_BUFFER, b.mesh->textures[1] );
			uniforms->scratchBase.Set( b.scratchBase );
			uniforms->faceBase.Set( b.faceBase );
			uniforms->numVerts.Set( b.numVerts );
			uniforms->numTris.Set( b.numTris );
			vaState.BindVertexBufferAndSetVertexFormat( b.mesh->vbo, VF_SKINNED );
			qglDrawArraysInstanced( GL_POINTS, 0, b.numVerts, b.count );
		}
		qglEndTransformFeedback();
	}

	qglBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	qglDisable( GL_RASTERIZER_DISCARD );

	// move results from staging to the places in vertex cache
	for ( const Batch &b : batches ) {
		for ( int k = 0; k < b.count; k++ ) {
			const Job &job = jobs[order[b.first + k]];
			const GLintptr src = (GLintptr)( b.stagingBase + k * b.numVerts ) * sizeof( idDrawVert );
			vertexCache.CopyToVertexRange( stagingBuffer, src, job.dst );
		}
	}
	qglBindBuffer( GL_COPY_READ_BUFFER, 0 );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );

	qglActiveTexture( GL_TEXTURE0 + savedTmu );
	jobs.clear();
	readyJoints.clear();
}

bool GpuSkinning::GetShadowTopology( vertCacheHandle_t mesh, int &numEdges, int &numTris ) {
	if ( !mesh.IsSkinnedBuffer() ) {
		return false;
	}
	std::lock_guard<std::mutex> lock( meshMutex );
	const int index = mesh.offset;
	if ( index < 0 || index >= (int)meshes.size() || !meshes[index]->inUse ) {
		return false;
	}
	const MeshData &d = meshes[index]->data;
	if ( d.numSilEdges <= 0 || d.silEdges == nullptr || d.numIndexes < 3 ) {
		return false;
	}
	numEdges = d.numSilEdges;
	numTris = d.numIndexes / 3;
	return true;
}

bool GpuSkinning::BeginShadowDraw( const float *projectionMatrix ) {
	if ( shadowProgram == nullptr ) {
		return false;
	}
	// the dynamic vertex buffer is read as texture buffer: floats, one per texel
	const GLuint buffer = vertexCache.GetDynamicVertexBufferObject();
	GLint64 bytes = 0;
	GLint maxTexels = 0;
	qglBindBuffer( GL_COPY_READ_BUFFER, buffer );
	qglGetBufferParameteri64v( GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &bytes );
	qglBindBuffer( GL_COPY_READ_BUFFER, 0 );
	qglGetIntegerv( GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels );
	if ( buffer == 0 || bytes <= 0 || bytes / 4 > maxTexels ) {
		if ( !shadowWarned ) {
			shadowWarned = true;
			common->Warning( "r_skinGpuShadows: dynamic vertex buffer (%d bytes) cannot be read as texture buffer (max %d texels), shadows of skinned models are skipped", (int)bytes, maxTexels );
		}
		return false;
	}
	const int savedTmu = backEnd.glState.currenttmu;
	if ( shadowVertTexture == 0 ) {
		qglGenTextures( 1, &shadowVertTexture );
	}
	// the buffer object can be recreated when vertex cache is resized, so attach it every time
	qglActiveTexture( GL_TEXTURE0 + SHADOW_VERT_UNIT );
	qglBindTexture( GL_TEXTURE_BUFFER, shadowVertTexture );
	qglTexBuffer( GL_TEXTURE_BUFFER, GL_R32F, buffer );
	qglActiveTexture( GL_TEXTURE0 + savedTmu );

	shadowProgram->Activate();
	SkinShadowUniforms *uniforms = shadowProgram->GetUniformGroup<SkinShadowUniforms>();
	uniforms->projectionMatrix.Set( projectionMatrix );
	return true;
}

void GpuSkinning::DrawShadowVolume( const drawSurf_t *surf, const idVec3 &localLightOrigin ) {
	if ( !surf->shadowCache.IsSkinnedBuffer() || !surf->ambientCache.IsValid() || surf->ambientCache.isStatic ) {
		return;
	}
	GLuint edgeTexture, indexTexture;
	int numEdges, numTris;
	{
		std::lock_guard<std::mutex> lock( meshMutex );
		const int index = surf->shadowCache.offset;
		if ( index < 0 || index >= (int)meshes.size() || !meshes[index]->inUse ) {
			return;
		}
		MeshEntry &mesh = *meshes[index];
		EnsureGl( mesh );
		edgeTexture = mesh.textures[3];
		indexTexture = mesh.textures[2];
		numEdges = mesh.data.numSilEdges;
		numTris = mesh.data.numIndexes / 3;
	}
	if ( surf->numIndexes <= 0 ) {
		return;
	}
	const int savedTmu = backEnd.glState.currenttmu;
	qglActiveTexture( GL_TEXTURE0 + SHADOW_EDGE_UNIT );
	qglBindTexture( GL_TEXTURE_BUFFER, edgeTexture );
	qglActiveTexture( GL_TEXTURE0 + SHADOW_INDEX_UNIT );
	qglBindTexture( GL_TEXTURE_BUFFER, indexTexture );
	qglActiveTexture( GL_TEXTURE0 + savedTmu );

	SkinShadowUniforms *uniforms = shadowProgram->GetUniformGroup<SkinShadowUniforms>();
	uniforms->modelViewMatrix.Set( surf->space->modelViewMatrix );
	uniforms->localLightOrigin.Set( idVec4( localLightOrigin, 0.0f ) );
	uniforms->vertBase.Set( (int)( surf->ambientCache.offset / sizeof( float ) ) );
	uniforms->numEdges.Set( numEdges );
	uniforms->numTris.Set( numTris );

	// surf->numIndexes is number of vertices here: sides only, or sides and caps
	qglDrawArrays( GL_TRIANGLES, 0, surf->numIndexes );
}

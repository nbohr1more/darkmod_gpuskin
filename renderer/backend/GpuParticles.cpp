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
#include "renderer/backend/GpuParticles.h"
#include "renderer/backend/GpuSkinning.h"
#include "renderer/backend/GLSLProgram.h"
#include "renderer/backend/GLSLProgramManager.h"
#include "renderer/backend/GLSLUniforms.h"
#include "framework/DeclParticle.h"
#include "framework/DeclTable.h"

GpuParticles gpuParticles;

struct ParticleGenUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( ParticleGenUniforms )

	DEFINE_UNIFORM( sampler, params )
	DEFINE_UNIFORM( sampler, tables )
	DEFINE_UNIFORM( int, paramBase )
};

static const int PARAM_TEXTURE_UNIT = 4;
static const int TABLE_TEXTURE_UNIT = 5;

namespace {
// serializes parameters in exactly the order which readParams() of particle_gen.vert.glsl reads them
struct ParamWriter {
	std::vector<int> &ints;
	std::vector<float> &tables;
	std::unordered_map<const idDeclTable *, std::pair<int, int>> &tableMap;

	void I( int v ) { ints.push_back( v ); }
	void F( float v ) {
		int bits;
		memcpy( &bits, &v, sizeof( bits ) );
		ints.push_back( bits );
	}
	void V3( const idVec3 &v ) { F( v.x ); F( v.y ); F( v.z ); }
	void V4( const idVec4 &v ) { F( v.x ); F( v.y ); F( v.z ); F( v.w ); }
	void Parm( const idParticleParm &p ) {
		F( p.from );
		F( p.to );
		int offset = -1, num = 0, flags = 0;
		if ( p.table ) {
			auto it = tableMap.find( p.table );
			if ( it == tableMap.end() ) {
				const float *values = nullptr;
				int count = 0;
				bool isClamp = false, isSnap = false;
				p.table->GetRawData( values, count, isClamp, isSnap );
				it = tableMap.emplace( p.table, std::make_pair( (int)tables.size(), count ) ).first;
				tables.insert( tables.end(), values, values + count );
				// flags are the same for the table every time, but are stored with parameter
			}
			const float *values = nullptr;
			int count = 0;
			bool isClamp = false, isSnap = false;
			p.table->GetRawData( values, count, isClamp, isSnap );
			offset = it->second.first;
			num = it->second.second;
			flags = ( isClamp ? 1 : 0 ) | ( isSnap ? 2 : 0 );
		}
		I( offset );
		I( num );
		I( flags );
	}
};
}

vertCacheHandle_t GpuParticles::AddJob( const srfTriangles_t *tri ) {
	const gpuParticleSystem_t *sys = tri->particleGpu;
	if ( sys == nullptr || sys->stage == nullptr || tri->numVerts <= 0 ) {
		return NO_CACHE;
	}
	const idParticleStage &stg = *sys->stage;
	const int quads = stg.NumQuadsPerParticle();
	if ( stg.totalParticles <= 0 || quads <= 0 || tri->numVerts != 4 * quads * stg.totalParticles ) {
		return NO_CACHE;
	}

	std::lock_guard<std::mutex> lock( jobMutex );

	vertCacheHandle_t dst = vertexCache.AllocVertexForGpuWrite( tri->numVerts * sizeof( idDrawVert ) );
	if ( !dst.IsValid() ) {
		return NO_CACHE;
	}

	// every job starts at a texel
	while ( pendingParams.size() & 3 ) {
		pendingParams.push_back( 0 );
	}
	Job job;
	job.dst = dst;
	job.numVerts = tri->numVerts;
	job.paramBase = (int)( pendingParams.size() / 4 );

	ParamWriter w = { pendingParams, pendingTables, pendingTableMap };
	// the order must match particle_gen.vert.glsl: readParamsEmit, readParamsColor, readParamsRest
	w.I( stg.totalParticles );
	w.F( stg.cycles );
	w.I( stg.diversityPeriod );
	w.I( stg.cycleMsec );
	w.F( stg.spawnBunching );
	w.F( stg.particleLife );
	w.F( stg.timeOffset );
	w.I( sys->viewTimeMs );
	w.F( sys->entityParmsTimeOffset );
	w.F( sys->entityParmsStopTime );
	w.F( sys->randomizer );
	w.I( quads );

	w.F( stg.fadeInFraction );
	w.F( stg.fadeOutFraction );
	w.F( stg.fadeIndexFraction );
	w.I( stg.entityColor ? 1 : 0 );
	w.V4( stg.color );
	w.V4( stg.fadeColor );
	w.V4( sys->entityParmsColor );

	w.I( (int)stg.distributionType );
	for ( int i = 0; i < 4; i++ ) w.F( stg.distributionParms[i] );
	w.I( (int)stg.directionType );
	for ( int i = 0; i < 4; i++ ) w.F( stg.directionParms[i] );
	w.Parm( stg.speed );
	w.F( stg.gravity );
	w.I( stg.worldGravity ? 1 : 0 );
	w.I( stg.worldAxis ? 1 : 0 );
	w.I( stg.randomDistribution ? 1 : 0 );
	w.I( (int)stg.customPathType );
	for ( int i = 0; i < 8; i++ ) w.F( stg.customPathParms[i] );
	w.V3( stg.offset );
	w.I( stg.animationFrames );
	w.F( stg.animationRate );
	w.F( stg.initialAngle );
	w.Parm( stg.rotationSpeed );
	w.I( (int)stg.orientation );
	for ( int i = 0; i < 4; i++ ) w.F( stg.orientationParms[i] );
	w.Parm( stg.size );
	w.Parm( stg.aspect );
	for ( int i = 0; i < 3; i++ ) w.V3( sys->entityAxis[i] );
	for ( int i = 0; i < 3; i++ ) w.V3( sys->viewAxis[i] );

	pendingJobs.push_back( job );
	return dst;
}

void GpuParticles::EndFrame() {
	// same as GpuSkinning::EndFrame: jobs of the completed frame are given to the backend
	std::lock_guard<std::mutex> lock( jobMutex );
	readyJobs.swap( pendingJobs );
	pendingJobs.clear();
	readyParams.swap( pendingParams );
	pendingParams.clear();
	readyTables.swap( pendingTables );
	pendingTables.clear();
	pendingTableMap.clear();
}

void GpuParticles::Init() {
	static_assert( sizeof( idDrawVert ) == 60, "transform feedback in particle_gen.vert.glsl writes 15 words" );
	program = programManager->LoadFromGenerator( "particle_gen", []( GLSLProgram *program ) {
		program->AttachVertexShader( "stages/particles/particle_gen.vert.glsl" );
		const char *varyings[] = { "tf_xyz", "tf_st", "tf_normal", "tf_tangent0", "tf_tangent1", "tf_color" };
		program->SetTransformFeedbackVaryings( 6, varyings );
		program->Link();
		ParticleGenUniforms *uniforms = program->GetUniformGroup<ParticleGenUniforms>();
		uniforms->params.Set( PARAM_TEXTURE_UNIT );
		uniforms->tables.Set( TABLE_TEXTURE_UNIT );
		uniforms->paramBase.Set( 0 );
	} );
}

void GpuParticles::Shutdown() {
	if ( stagingBuffer ) {
		qglDeleteBuffers( 1, &stagingBuffer );
		stagingBuffer = 0;
		stagingCapacity = 0;
	}
	if ( paramBuffer ) {
		qglDeleteBuffers( 1, &paramBuffer );
		qglDeleteTextures( 1, &paramTexture );
		paramBuffer = paramTexture = 0;
	}
	if ( tableBuffer ) {
		qglDeleteBuffers( 1, &tableBuffer );
		qglDeleteTextures( 1, &tableTexture );
		tableBuffer = tableTexture = 0;
	}
	std::lock_guard<std::mutex> lock( jobMutex );
	pendingJobs.clear();
	readyJobs.clear();
	pendingParams.clear();
	readyParams.clear();
	pendingTables.clear();
	readyTables.clear();
	pendingTableMap.clear();
}

void GpuParticles::EnsureStaging( int numVerts ) {
	if ( numVerts <= stagingCapacity && stagingBuffer != 0 ) {
		return;
	}
	int newCapacity = std::max( numVerts, 4096 );
	newCapacity = std::max( newCapacity, stagingCapacity * 2 );
	if ( stagingBuffer == 0 ) {
		qglGenBuffers( 1, &stagingBuffer );
		GL_SetDebugLabel( GL_BUFFER, stagingBuffer, "ParticlesStaging" );
	}
	qglBindBuffer( GL_COPY_WRITE_BUFFER, stagingBuffer );
	qglBufferData( GL_COPY_WRITE_BUFFER, (GLsizeiptr)newCapacity * sizeof( idDrawVert ), nullptr, GL_DYNAMIC_COPY );
	qglBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
	stagingCapacity = newCapacity;
}

void GpuParticles::UploadTexelBuffer( GLuint &buffer, GLuint &texture, GLenum format, const void *data, GLsizeiptr bytes ) {
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

void GpuParticles::ReportTimers() {
	const int period = r_skinTimers.GetInteger();
	if ( period <= 0 || timerCalls < period ) {
		return;
	}
	const double n = timerCalls;
	const long long ns = skinTimers.particleNs.exchange( 0 );
	const long long cs = skinTimers.particleCpuStages.exchange( 0 ), cv = skinTimers.particleCpuVerts.exchange( 0 );
	const long long gs = skinTimers.particleGpuStages.exchange( 0 ), gv = skinTimers.particleGpuVerts.exchange( 0 );
	if ( cs > 0 || gs > 0 ) {
		common->Printf( "particleTimers (avg per call, %d calls): frontend InstantiateDynamicModel %.0f us | stages: cpu %.1f (%.0f verts) gpu %.1f (%.0f verts) | backend: Execute cpu %.0f us gpu %.0f us, jobs %.1f, verts %.0f\n",
			timerCalls, ns / 1000.0 / n, cs / n, cv / n, gs / n, gv / n, executeCpuNs / 1000.0 / n, gpuNs / 1000.0 / n, timerJobs / n, timerVerts / n );
	}
	timerCalls = timerJobs = timerVerts = 0;
	gpuNs = executeCpuNs = 0;
}

void GpuParticles::Execute() {
	const bool timing = r_skinTimers.GetInteger() > 0;
	long long startNs = 0;
	bool queryStarted = false;
	if ( timing ) {
		startNs = SkinNowNs();
		timerCalls++;
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
	struct Finish {
		GpuParticles *self; bool timing; long long start; bool *queryStarted;
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
	if ( jobs.empty() || program == nullptr ) {
		readyParams.clear();
		readyTables.clear();
		jobs.clear();
		return;
	}
	TRACE_GL_SCOPE( "GpuParticles" );
	if ( timing && !timerPending[timerIndex] ) {
		if ( timerQueries[timerIndex] == 0 ) {
			qglGenQueries( 4, timerQueries );
		}
		qglBeginQuery( GL_TIME_ELAPSED, timerQueries[timerIndex] );
		queryStarted = true;
	}

	int totalVerts = 0;
	for ( const Job &job : jobs ) {
		totalVerts += job.numVerts;
	}
	timerJobs += (int)jobs.size();
	timerVerts += totalVerts;
	EnsureStaging( totalVerts );

	const int savedTmu = backEnd.glState.currenttmu;
	qglEnable( GL_RASTERIZER_DISCARD );

	qglActiveTexture( GL_TEXTURE0 + PARAM_TEXTURE_UNIT );
	UploadTexelBuffer( paramBuffer, paramTexture, GL_RGBA32I, readyParams.data(), (GLsizeiptr)readyParams.size() * sizeof( int ) );
	qglActiveTexture( GL_TEXTURE0 + TABLE_TEXTURE_UNIT );
	static const float dummyTable[1] = { 0.0f };
	if ( readyTables.empty() ) {
		UploadTexelBuffer( tableBuffer, tableTexture, GL_R32F, dummyTable, sizeof( dummyTable ) );
	} else {
		UploadTexelBuffer( tableBuffer, tableTexture, GL_R32F, readyTables.data(), (GLsizeiptr)readyTables.size() * sizeof( float ) );
	}

	program->Activate();
	ParticleGenUniforms *uniforms = program->GetUniformGroup<ParticleGenUniforms>();
	qglBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, stagingBuffer, 0, (GLsizeiptr)totalVerts * sizeof( idDrawVert ) );
	qglBeginTransformFeedback( GL_POINTS );
	for ( const Job &job : jobs ) {
		uniforms->paramBase.Set( job.paramBase );
		qglDrawArrays( GL_POINTS, 0, job.numVerts );
	}
	qglEndTransformFeedback();
	qglBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	qglDisable( GL_RASTERIZER_DISCARD );

	GLintptr src = 0;
	vertexCache.BeginVertexCopies( stagingBuffer );
	for ( const Job &job : jobs ) {
		vertexCache.CopyToVertexRange( src, job.dst );
		src += (GLintptr)job.numVerts * sizeof( idDrawVert );
	}
	vertexCache.EndVertexCopies();

	qglActiveTexture( GL_TEXTURE0 + savedTmu );
	jobs.clear();
	readyParams.clear();
	readyTables.clear();
}

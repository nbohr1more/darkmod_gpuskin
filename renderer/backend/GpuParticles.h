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
#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>
#include "renderer/tr_local.h"

class GLSLProgram;
class idParticleStage;
class idDeclTable;

/**
 * GPU generation of particle models (r_particlesGpu).
 *
 * The frontend does not compute particles: for every particle stage it only checks if anything is alive, and queues
 * a job with the parameters of the stage and the system (gpuParticleSystem_t). The backend runs all jobs of the frame
 * in one transform feedback pass (glprogs/stages/particles/particle_gen.vert.glsl, which is a port of ParticleSystem_def.h)
 * which writes ordinary idDrawVert quads into the dynamic vertex cache.
 * Dead particles are degenerate quads. Only unlit stages without cutoff maps use this (see idRenderModelPrt).
 */

// parameters of one particle stage of one entity, valid during the frame of the frontend
struct gpuParticleSystem_t {
	const idParticleStage *stage;
	idMat3 entityAxis;
	idMat3 viewAxis;
	idVec4 entityParmsColor;
	int viewTimeMs;
	float entityParmsTimeOffset;
	float entityParmsStopTime;
	float randomizer;
};

class GpuParticles {
public:
	// frontend: schedules generation of the particles of the surface, returns handle in the dynamic vertex cache (filled by GPU later)
	// tri->particleGpu must be set, tri->numVerts must be 4 * total number of quads
	vertCacheHandle_t AddJob( const srfTriangles_t *tri );

	// called by vertex cache when it switches to the next frame
	void EndFrame();

	// backend (GL context thread)
	void Init();
	void Shutdown();
	void Execute();

private:
	struct Job {
		vertCacheHandle_t dst;
		int numVerts;
		int paramBase;		// texel index in params
	};

	std::mutex jobMutex;
	std::vector<Job> pendingJobs, readyJobs;
	std::vector<int> pendingParams, readyParams;		// serialized parameters (see AddJob and particle_gen.vert.glsl)
	std::vector<float> pendingTables, readyTables;		// values of tables
	std::unordered_map<const idDeclTable *, std::pair<int, int>> pendingTableMap;	// table -> offset, count

	GLSLProgram *program = nullptr;
	GLuint paramBuffer = 0, paramTexture = 0;
	GLuint tableBuffer = 0, tableTexture = 0;
	GLuint stagingBuffer = 0;
	int stagingCapacity = 0;		// in vertices

	// r_skinTimers
	GLuint timerQueries[4] = { 0, 0, 0, 0 };
	bool timerPending[4] = { false, false, false, false };
	int timerIndex = 0;
	long long gpuNs = 0, executeCpuNs = 0;
	int timerCalls = 0, timerJobs = 0, timerVerts = 0;
	void ReportTimers();

	void EnsureStaging( int numVerts );
	void UploadTexelBuffer( GLuint &buffer, GLuint &texture, GLenum format, const void *data, GLsizeiptr bytes );
};

extern GpuParticles gpuParticles;

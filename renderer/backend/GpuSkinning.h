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

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include "renderer/tr_local.h"

class GLSLProgram;

/**
 * GPU skinning of MD5 meshes.
 *
 * Skinning is a pre-pass which runs once per frame in the backend, before anything is drawn:
 * it produces ordinary idDrawVert vertices (with exact smooth normals and tangents) in the
 * dynamic vertex cache, so all the other rendering code and shaders work with skinned surfaces
 * exactly as they work with CPU-skinned ones.
 *
 * Every mesh has a static "skinned mesh" (see idDrawVertSkinned) with rest-pose data, joint indices/weights
 * and per-vertex triangle adjacency. Every visible surface of an animated model is a "job":
 *   frontend: AddJob reserves space in the dynamic vertex cache and queues the joint matrices
 *   backend:  Execute uploads all joint matrices of the frame at once (texture buffer), groups jobs by mesh,
 *             and runs every pass as ONE instanced draw per group (instance = surface), so the cost
 *             does not depend on the number of surfaces much. Results are written to a staging buffer
 *             and then copied to the vertex cache ranges of the surfaces.
 *             Passes for surfaces which need lighting:
 *     1) skin positions (skin_positions.vert.glsl)
 *     2) per triangle: unit normal and tangents of every skinned triangle (skin_faces.vert.glsl)
 *     3) per vertex: gather face data, write idDrawVert into the vertex cache (skin_tangents.vert.glsl)
 *   Surfaces which are drawn without lighting need only positions, so a single pass (skin_direct.vert.glsl)
 *   writes idDrawVert with rest pose normals/tangents.
 *
 * The frontend runs one frame ahead of the backend, so jobs are queued until vertex cache switches the frame.
 */
// r_skinTimers: accumulated time spent on skinning, reported by GpuSkinning::Execute
struct SkinTimers {
	std::atomic<long long> positionsNs{0};		// CPU: TransformVerts + mirrored verts (frontend)
	std::atomic<long long> boundsNs{0};			// CPU: R_BoundTriSurf (frontend)
	std::atomic<long long> tangentsNs{0};		// CPU: R_DeriveTangents inside UpdateSurface (frontend, CPU skinning only)
	std::atomic<long long> jointsNs{0};			// CPU: building joint matrices for GPU skinning (frontend)
	std::atomic<long long> ambientNs{0};		// CPU: R_CreateAmbientCache of deformed surfaces: GPU job queueing, or deferred tangents + vertex upload
	// frontend work on deformed surfaces which needs CPU positions (excluding time of lazy position fill)
	std::atomic<long long> facingNs{0};			// R_CalcInteractionFacing: which triangles face the light
	std::atomic<long long> cullBitsNs{0};		// R_CalcInteractionCullBits
	std::atomic<long long> lightTrisNs{0};		// R_CreateLightTris: surface clipped to the light
	std::atomic<long long> shadowNs{0};			// shadow volume / shadow cache creation
	std::atomic<long long> cpuSurfaces{0}, cpuVerts{0};
	std::atomic<long long> gpuSurfaces{0}, gpuVerts{0};
	// why surfaces were skinned on CPU (surface counts)
	std::atomic<long long> whyScale{0};			// entity uses skin scale
	std::atomic<long long> whyModel{0};			// model is not set up for GPU (too many joints, ...)
	std::atomic<long long> whyDominant{0};		// unsmoothedTangents material
	std::atomic<long long> whyNoMesh{0};		// no skinned mesh was built (unsupported vertex duplication, ...)
};
extern SkinTimers skinTimers;
inline long long SkinNowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}
// adds elapsed time of the scope to the counter, if enabled
struct SkinScopeTimer {
	std::atomic<long long> *dst;
	long long start;
	SkinScopeTimer( std::atomic<long long> &counter, bool enabled ) : dst( enabled ? &counter : nullptr ), start( enabled ? SkinNowNs() : 0 ) {}
	~SkinScopeTimer() { if ( dst ) *dst += SkinNowNs() - start; }
};

class GpuSkinning {
public:
	// static data of a mesh, all arrays must stay valid until FreeMesh
	struct MeshData {
		const idDrawVertSkinned *verts = nullptr;
		int numVerts = 0;
		const int *vertInfo = nullptr;		// 4 ints per vertex
		const int *triLists = nullptr;
		int numTriLists = 0;
		const glIndex_t *indexes = nullptr;	// triangles
		int numIndexes = 0;
		const int *silEdges = nullptr;		// 4 ints per edge: triangle1, triangle2, vertex1, vertex2 (silEdge_t), for GPU shadow volumes
		int numSilEdges = 0;
	};

	// load time (any thread): registers a mesh, returns a static handle (see vertCacheHandle_t::IsSkinnedBuffer)
	vertCacheHandle_t AllocMesh( const MeshData &data );
	void FreeMesh( vertCacheHandle_t handle );

	// frontend: schedules skinning of the surface this frame
	// returns handle of vertices in dynamic vertex cache (they are filled by GPU later), or NO_CACHE if failed
	// if needsLighting is false, then normals and tangents are not computed (see UpgradeJob)
	vertCacheHandle_t AddJob( const srfTriangles_t *tri, bool needsLighting );
	// frontend: the surface which was added without lighting is going to be lit: compute normals and tangents too
	// handle must be the one returned by AddJob during this frame
	bool UpgradeJob( vertCacheHandle_t handle );

	// GPU shadow volumes (r_skinGpuShadows)
	// frontend: gets numbers of silhouette edges and triangles of the mesh, returns false if shadows cannot be done on GPU
	bool GetShadowTopology( vertCacheHandle_t mesh, int &numEdges, int &numTris );
	// backend: prepares drawing of shadow volumes of GPU-skinned surfaces, returns false if not possible
	bool BeginShadowDraw( const float *projectionMatrix );
	// backend: draws shadow volume of the surface, surf->shadowCache is handle of the mesh, surf->ambientCache is skinned vertices
	void DrawShadowVolume( const drawSurf_t *surf, const idVec3 &localLightOrigin );

	// called by vertex cache when it switches to the next frame
	void EndFrame();

	// backend (GL context thread)
	void Init();
	void Shutdown();
	void Execute();

private:
	struct MeshEntry {
		bool inUse = false;
		MeshData data;
		// GL objects are created lazily by the backend
		GLuint vbo = 0;
		GLuint buffers[4] = { 0, 0, 0, 0 };	// vertInfo, triLists, indexes, silEdges
		GLuint textures[4] = { 0, 0, 0, 0 };
		bool glReady = false;
	};
	struct Job {
		MeshEntry *mesh;
		int numVerts;
		int jointBase;		// index of the first joint in the joint array of the frame
		vertCacheHandle_t dst;
		bool lit;			// normals and tangents are needed
		bool valid;
	};
	struct DeletedGl {
		GLuint vbo;
		GLuint buffers[4];
		GLuint textures[4];
	};

	std::mutex meshMutex;
	std::vector<MeshEntry *> meshes;
	std::vector<int> freeMeshes;
	std::vector<DeletedGl> pendingDelete;

	std::mutex jobMutex;
	std::vector<Job> pendingJobs;		// being filled by frontend
	std::vector<Job> readyJobs;			// to be executed by backend
	std::vector<float> pendingJoints;	// 12 floats per joint (idJointMat), being filled by frontend
	std::vector<float> readyJoints;
	int frameCounter = 0;				// increased by EndFrame, see skinJointSet_t::frame
	int maxJointTexels = 0;

	GLSLProgram *positionsProgram = nullptr;
	GLSLProgram *facesProgram = nullptr;
	GLSLProgram *tangentsProgram = nullptr;
	GLSLProgram *directProgram = nullptr;
	GLSLProgram *shadowProgram = nullptr;
	GLuint shadowVertTexture = 0;		// dynamic vertex buffer as texture buffer
	bool shadowWarned = false;
	GLuint scratchBuffer = 0;
	GLuint scratchTexture = 0;
	int scratchCapacity = 0;			// in vertices
	GLuint faceBuffer = 0;
	GLuint faceTexture = 0;
	int faceCapacity = 0;				// in triangles
	GLuint stagingBuffer = 0;
	int stagingCapacity = 0;			// in vertices
	GLuint jointBuffer = 0, jointTexture = 0;

	// r_skinTimers
	GLuint timerQueries[4] = { 0, 0, 0, 0 };
	bool timerPending[4] = { false, false, false, false };
	int timerIndex = 0;
	long long gpuNs = 0, executeCpuNs = 0;
	int timerCalls = 0, timerJobs = 0, timerBatches = 0;
	void ReportTimers();
	GLuint instanceBuffer = 0, instanceTexture = 0;

	void EnsureGl( MeshEntry &mesh );
	void EnsureScratch( int numVerts );
	void EnsureFaces( int numTris );
	void EnsureStaging( int numVerts );
	void UploadTexelBuffer( GLuint &buffer, GLuint &texture, GLenum format, const void *data, GLsizeiptr bytes );
	void DeleteGl( const DeletedGl &del );
	void DeletePending();
	void LoadPositionsProgram( GLSLProgram *program );
	void LoadFacesProgram( GLSLProgram *program );
	void LoadTangentsProgram( GLSLProgram *program );
	void LoadDirectProgram( GLSLProgram *program );
	void LoadShadowProgram( GLSLProgram *program );
};

extern GpuSkinning gpuSkinning;

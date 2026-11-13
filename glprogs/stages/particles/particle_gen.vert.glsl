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

// GPU generation of particle systems (particle models, .prt), see GpuParticles.cpp.
// This is a port of ParticleSystem_def.h: keep them in sync!
// One shader invocation = one vertex of the output buffer (idDrawVert, 60 bytes), captured by transform feedback.
// Vertex number = ((particle * quadsPerParticle) + quad) * 4 + cornerOfQuad
// Dead particles produce degenerate quads (all vertices equal, zero color).

uniform isamplerBuffer u_params;	// serialized parameters of all systems of the frame, one texel = 4 ints (floats are stored as bits)
uniform samplerBuffer u_tables;		// values of all tables (idDeclTable) used by the frame
uniform int u_paramBase;			// first texel of parameters of this system

// layout and order must match idDrawVert
out vec3 tf_xyz;
out vec2 tf_st;
out vec3 tf_normal;
out vec3 tf_tangent0;
out vec3 tf_tangent1;
flat out uint tf_color;

// ---------------- reading of parameters (order must match GpuParticles.cpp) ----------------

int pc;	// cursor, in ints

int nextI() {
	int v = texelFetch(u_params, u_paramBase + (pc >> 2))[pc & 3];
	pc++;
	return v;
}
float nextF() {
	return intBitsToFloat(nextI());
}
vec3 nextV3() {
	float x = nextF();
	float y = nextF();
	float z = nextF();
	return vec3(x, y, z);
}
vec4 nextV4() {
	float x = nextF();
	float y = nextF();
	float z = nextF();
	float w = nextF();
	return vec4(x, y, z, w);
}

struct Parm {
	float from;
	float to;
	int tableOffset;	// -1 if no table
	int tableNum;
	int flags;			// 1 = clamp, 2 = snap
};
Parm nextParm() {
	Parm p;
	p.from = nextF();
	p.to = nextF();
	p.tableOffset = nextI();
	p.tableNum = nextI();
	p.flags = nextI();
	return p;
}

const int PDIST_RECT = 0;
const int PDIST_CYLINDER = 1;
const int PDIST_SPHERE = 2;
const int PDIR_CONE = 0;
const int PDIR_OUTWARD = 1;
const int PPATH_STANDARD = 0;
const int PPATH_HELIX = 1;
const int PPATH_FLIES = 2;
const int PPATH_ORBIT = 3;
const int PPATH_DRIP = 4;
const int POR_VIEW = 0;
const int POR_AIMED = 1;
const int POR_X = 2;
const int POR_Y = 3;
const int POR_Z = 4;
const float PI = 3.14159265358979323846;

// idPartStageData
int s_totalParticles;
float s_cycles;
int s_diversityPeriod;
int s_cycleMsec;
float s_spawnBunching;
float s_particleLife;
float s_timeOffset;
int s_distributionType;
vec4 s_distributionParms;
int s_directionType;
vec4 s_directionParms;
Parm s_speed;
float s_gravity;
bool s_worldGravity;
bool s_worldAxis;
bool s_randomDistribution;
bool s_entityColor;
int s_customPathType;
float s_customPathParms[8];
vec3 s_offset;
int s_animationFrames;
float s_animationRate;
float s_initialAngle;
Parm s_rotationSpeed;
int s_orientation;
vec4 s_orientationParms;
Parm s_size;
Parm s_aspect;
vec4 s_color;
vec4 s_fadeColor;
float s_fadeInFraction;
float s_fadeOutFraction;
float s_fadeIndexFraction;
// idPartSysData and idPartSysEmit
vec3 s_entityAxisRow0, s_entityAxisRow1, s_entityAxisRow2;
vec3 s_viewAxisRow0, s_viewAxisRow1, s_viewAxisRow2;
vec4 s_entityParmsColor;
int s_viewTimeMs;
float s_entityParmsTimeOffset;
float s_entityParmsStopTime;
float s_randomizer;
int s_quadsPerParticle;

void readParams() {
	pc = 0;
	s_totalParticles = nextI();
	s_cycles = nextF();
	s_diversityPeriod = nextI();
	s_cycleMsec = nextI();
	s_spawnBunching = nextF();
	s_particleLife = nextF();
	s_timeOffset = nextF();
	s_distributionType = nextI();
	s_distributionParms = nextV4();
	s_directionType = nextI();
	s_directionParms = nextV4();
	s_speed = nextParm();
	s_gravity = nextF();
	s_worldGravity = nextI() != 0;
	s_worldAxis = nextI() != 0;
	s_randomDistribution = nextI() != 0;
	s_entityColor = nextI() != 0;
	s_customPathType = nextI();
	for (int i = 0; i < 8; i++)
		s_customPathParms[i] = nextF();
	s_offset = nextV3();
	s_animationFrames = nextI();
	s_animationRate = nextF();
	s_initialAngle = nextF();
	s_rotationSpeed = nextParm();
	s_orientation = nextI();
	s_orientationParms = nextV4();
	s_size = nextParm();
	s_aspect = nextParm();
	s_color = nextV4();
	s_fadeColor = nextV4();
	s_fadeInFraction = nextF();
	s_fadeOutFraction = nextF();
	s_fadeIndexFraction = nextF();
	s_entityAxisRow0 = nextV3();
	s_entityAxisRow1 = nextV3();
	s_entityAxisRow2 = nextV3();
	s_viewAxisRow0 = nextV3();
	s_viewAxisRow1 = nextV3();
	s_viewAxisRow2 = nextV3();
	s_entityParmsColor = nextV4();
	s_viewTimeMs = nextI();
	s_entityParmsTimeOffset = nextF();
	s_entityParmsStopTime = nextF();
	s_randomizer = nextF();
	s_quadsPerParticle = nextI();
}

// ---------------- idRandom ----------------

// same as idRandom::RandomFloat: seed is advanced
float rndFloat(inout uint seed) {
	seed = seed * 1103515245u + 12345u;
	return float((seed >> 16u) & 0x7fffu) / 32768.0;
}
float crndFloat(inout uint seed) {
	return 2.0 * (rndFloat(seed) - 0.5);
}

// ---------------- tables and parameters ----------------

float tableLookup(Parm p, float index) {
	int domain = p.tableNum - 1;
	if (domain <= 1)
		return 1.0;
	bool isClamp = (p.flags & 1) != 0;
	bool isSnap = (p.flags & 2) != 0;
	int iIndex;
	float iFrac;
	if (isClamp) {
		index *= float(domain - 1);
		if (index >= float(domain - 1))
			return texelFetch(u_tables, p.tableOffset + domain - 1).x;
		else if (index <= 0.0)
			return texelFetch(u_tables, p.tableOffset).x;
		iIndex = int(index);
		iFrac = index - float(iIndex);
	} else {
		index *= float(domain);
		if (index < 0.0)
			index += float(domain) * ceil(-index / float(domain));
		iIndex = int(floor(index));
		iFrac = index - float(iIndex);
		iIndex = iIndex % domain;
	}
	if (!isSnap) {
		float a = texelFetch(u_tables, p.tableOffset + iIndex).x;
		float b = texelFetch(u_tables, p.tableOffset + iIndex + 1).x;
		return a * (1.0 - iFrac) + b * iFrac;
	}
	return texelFetch(u_tables, p.tableOffset + iIndex).x;
}

float parmEval(Parm p, float frac) {
	if (p.tableOffset >= 0)
		return tableLookup(p, frac);
	return p.from + frac * (p.to - p.from);
}

float parmIntegrate(Parm p, float frac) {
	if (p.tableOffset >= 0)
		return 0.0;	// integration of tables is not supported
	return (p.from + frac * (p.to - p.from) * 0.5) * frac;
}

// idMat3 * vec3 (rows of the matrix are given)
vec3 mulRows(vec3 r0, vec3 r1, vec3 r2, vec3 v) {
	return vec3(dot(r0, v), dot(r1, v), dot(r2, v));
}
// "v *= transpose(entityAxis)" of the CPU code
vec3 entityAxisMul(vec3 v) {
	return mulRows(s_entityAxisRow0, s_entityAxisRow1, s_entityAxisRow2, v);
}

vec3 safeNormalize(vec3 v) {
	float len = length(v);
	return len > 0.0 ? v / len : v;
}

// ---------------- particle origin ----------------

vec3 particleOriginStdSys(float frac, inout uint random) {
	vec3 origin = vec3(0.0);

	if (s_customPathType == PPATH_STANDARD) {
		float radiusSqr, angle1, angle2;

		if (s_distributionType == PDIST_RECT) {
			origin.x = (s_randomDistribution ? crndFloat(random) : 1.0) * s_distributionParms[0];
			origin.y = (s_randomDistribution ? crndFloat(random) : 1.0) * s_distributionParms[1];
			origin.z = (s_randomDistribution ? crndFloat(random) : 1.0) * s_distributionParms[2];
		} else if (s_distributionType == PDIST_CYLINDER) {
			angle1 = (s_randomDistribution ? crndFloat(random) : 1.0) * (2.0 * PI);
			origin.x = sin(angle1);
			origin.y = cos(angle1);
			origin.z = (s_randomDistribution ? crndFloat(random) : 1.0);

			if (s_distributionParms[3] > 0.0) {
				radiusSqr = origin.x * origin.x + origin.y * origin.y;
				if (radiusSqr < s_distributionParms[3] * s_distributionParms[3]) {
					float f = sqrt(radiusSqr) / s_distributionParms[3];
					float invf = 1.0 / f;
					float newRadius = s_distributionParms[3] + f * (1.0 - s_distributionParms[3]);
					float rescale = invf * newRadius;
					origin.x *= rescale;
					origin.y *= rescale;
				}
			}
			origin.x *= s_distributionParms[0];
			origin.y *= s_distributionParms[1];
			origin.z *= s_distributionParms[2];
		} else if (s_distributionType == PDIST_SPHERE) {
			if (s_randomDistribution) {
				// rejection sampling, like the CPU code (iteration count is bounded for safety)
				for (int it = 0; it < 64; it++) {
					origin.x = crndFloat(random);
					origin.y = crndFloat(random);
					origin.z = crndFloat(random);
					radiusSqr = origin.x * origin.x + origin.y * origin.y + origin.z * origin.z;
					if (radiusSqr <= 1.0)
						break;
				}
			} else {
				origin = vec3(1.0, 1.0, 1.0);
				radiusSqr = 3.0;
			}

			if (s_distributionParms[3] > 0.0) {
				if (radiusSqr < s_distributionParms[3] * s_distributionParms[3]) {
					float f = sqrt(radiusSqr) / s_distributionParms[3];
					float invf = 1.0 / f;
					float newRadius = s_distributionParms[3] + f * (1.0 - s_distributionParms[3]);
					float rescale = invf * newRadius;
					origin.x *= rescale;
					origin.y *= rescale;
					origin.z *= rescale;
				}
			}
			origin.x *= s_distributionParms[0];
			origin.y *= s_distributionParms[1];
			origin.z *= s_distributionParms[2];
		}

		origin += s_offset;

		vec3 dir = vec3(0.0);
		if (s_directionType == PDIR_CONE) {
			if (s_directionParms[0] != 0.0) {
				angle1 = crndFloat(random) * s_directionParms[0] * (PI / 180.0);
				angle2 = crndFloat(random) * PI;

				float s1 = sin(angle1), c1 = cos(angle1);
				float s2 = sin(angle2), c2 = cos(angle2);

				dir.x = s1 * c2;
				dir.y = s1 * s2;
				dir.z = c1;
			} else {
				dir = vec3(0.0, 0.0, 1.0);
			}
		} else if (s_directionType == PDIR_OUTWARD) {
			dir = safeNormalize(origin);
			dir.z += s_directionParms[0];
		}

		float iSpeed = parmIntegrate(s_speed, frac);
		origin += dir * iSpeed * s_particleLife;
	} else {
		float age = frac * s_particleLife;

		float angle1, angle2, speed1, speed2;
		if (s_customPathType == PPATH_HELIX) {
			speed1 = crndFloat(random);
			speed2 = crndFloat(random);
			angle1 = rndFloat(random) * (2.0 * PI) + s_customPathParms[3] * speed1 * age;

			origin.x = cos(angle1) * s_customPathParms[0];
			origin.y = sin(angle1) * s_customPathParms[1];
			origin.z = rndFloat(random) * s_customPathParms[2] + s_customPathParms[4] * speed2 * age;
		} else if (s_customPathType == PPATH_FLIES) {
			speed1 = clamp(crndFloat(random), 0.4, 1.0);
			speed2 = clamp(crndFloat(random), 0.4, 1.0);
			angle1 = rndFloat(random) * (PI * 2.0) + s_customPathParms[0] * speed1 * age;
			angle2 = rndFloat(random) * (PI * 2.0) + s_customPathParms[1] * speed1 * age;

			float s1 = sin(angle1), c1 = cos(angle1);
			float s2 = sin(angle2), c2 = cos(angle2);

			origin.x = c1 * c2;
			origin.y = s1 * c2;
			origin.z = -s2;
			origin *= s_customPathParms[2];
		} else if (s_customPathType == PPATH_ORBIT) {
			angle1 = rndFloat(random) * (PI * 2.0) + s_customPathParms[1] * age;

			origin.x = cos(angle1) * s_customPathParms[0];
			origin.y = sin(angle1) * s_customPathParms[0];

			float radius = s_customPathParms[0];
			// same formula as the CPU code
			float rsqr = radius * radius;
			float len = length(origin);
			if (len < rsqr * 0.5) {
				origin.z = sqrt(rsqr - len);
			} else {
				origin.z = rsqr / (2.0 * sqrt(len));
			}
		} else if (s_customPathType == PPATH_DRIP) {
			origin.x = 0.0;
			origin.y = 0.0;
			origin.z = -(age * s_customPathParms[0]);
		}

		origin += s_offset;
	}

	return origin;
}

vec3 particleOrigin(float frac, inout uint random) {
	vec3 origin = particleOriginStdSys(frac, random);

	// particle models: part.axis is identity and part.origin is zero
	if (s_worldAxis) {
		origin = entityAxisMul(origin);
	}

	float age = frac * s_particleLife;

	if (s_worldGravity) {
		vec3 gra = entityAxisMul(vec3(0.0, 0.0, -s_gravity));
		origin += gra * age * age;
	} else {
		origin.z -= s_gravity * age * age;
	}

	return origin;
}

// ---------------- emitting ----------------

// returns false if the particle does not exist now
bool emitParticle(int index, out float frac, out uint randomSeed) {
	frac = -1000000.0;
	randomSeed = 0u;

	int stageAge = int(float(s_viewTimeMs) + s_entityParmsTimeOffset * 1000.0 - s_timeOffset * 1000.0);

	int bunchOffset = int(s_particleLife * 1000.0 * s_spawnBunching * float(index) / float(s_totalParticles));
	int particleAge = stageAge - bunchOffset;
	if (particleAge < 0) {
		// before the particle system spawned
		return false;
	}
	int particleCycle = particleAge / s_cycleMsec;

	if (s_cycles != 0.0 && float(particleCycle) >= s_cycles) {
		// cycled systems will only run cycle times
		return false;
	}

	int inCycleTime = particleAge - particleCycle * s_cycleMsec;
	if (s_entityParmsStopTime != 0.0 && float(s_viewTimeMs - inCycleTime) >= s_entityParmsStopTime * 1000.0) {
		// don't fire any more particles
		return false;
	}

	frac = float(inCycleTime) / (s_particleLife * 1000.0);
	if (frac < 0.0 || frac > 1.0) {
		// yet to be spawned or in the deadTime band
		return false;
	}

	int cycleSeed = particleCycle;
	if (s_diversityPeriod > 0)
		cycleSeed = cycleSeed % s_diversityPeriod;

	// unsigned arithmetic wraps: same as the (truncated) integer arithmetic of idParticle_GetRandomSeed
	randomSeed = uint(cycleSeed) * 1580030168u + uint(index * index) * 2654435769u + uint(int(s_randomizer * 46341.0));
	return true;
}

vec4 particleColors(float frac, int index) {
	float fadeFraction = 1.0;

	if (frac < s_fadeInFraction) {
		fadeFraction *= (frac / s_fadeInFraction);
	}
	if (1.0 - frac < s_fadeOutFraction) {
		fadeFraction *= ((1.0 - frac) / s_fadeOutFraction);
	}

	if (s_fadeIndexFraction != 0.0) {
		int totalParticles = s_totalParticles;
		float indexFrac = float(totalParticles - index) / float(totalParticles);
		if (indexFrac < s_fadeIndexFraction) {
			fadeFraction *= indexFrac / s_fadeIndexFraction;
		}
	}

	vec4 fcolor = (s_entityColor ? s_entityParmsColor : s_color) * fadeFraction + s_fadeColor * (1.0 - fadeFraction);
	return clamp(fcolor, 0.0, 1.0);
}

// texcoord of the corner: (s, t) of the first corner and (width, height) of the quad
void particleTexCoords(float frac, out float s, out float width, out float animationFrameFrac) {
	float age = frac * s_particleLife;
	animationFrameFrac = -1.0;

	if (s_animationFrames > 1) {
		width = 1.0 / float(s_animationFrames);
		float floatFrame;
		if (s_animationRate != 0.0) {
			floatFrame = age * s_animationRate;
		} else {
			floatFrame = frac * float(s_animationFrames);
		}
		int intFrame = int(floatFrame);
		animationFrameFrac = clamp(floatFrame - float(intFrame), 0.0, 1.0);
		s = width * float(intFrame);
	} else {
		s = 0.0;
		width = 1.0;
	}
}

void writeVertex(vec3 xyz, vec2 st, vec4 color) {
	tf_xyz = xyz;
	tf_st = st;
	tf_normal = vec3(0.0, 0.0, 1.0);
	tf_tangent0 = vec3(1.0, 0.0, 0.0);
	tf_tangent1 = vec3(0.0, 1.0, 0.0);
	// the CPU code truncates
	uvec4 c = uvec4(clamp(color, 0.0, 1.0) * 255.0);
	tf_color = c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
	gl_Position = vec4(0.0);
}

void writeDead() {
	writeVertex(vec3(0.0), vec2(0.0), vec4(0.0));
}

void main() {
	readParams();

	int vid = gl_VertexID;
	int corner = vid & 3;
	int quadIndex = vid >> 2;
	int particle = quadIndex / s_quadsPerParticle;
	int quad = quadIndex - particle * s_quadsPerParticle;

	float frac;
	uint seed;
	if (!emitParticle(particle, frac, seed)) {
		writeDead();
		return;
	}

	vec4 color = particleColors(frac, particle);
	// if we are completely faded out, kill the particle
	if (color == vec4(0.0)) {
		writeDead();
		return;
	}

	uint random = seed;
	vec3 origin = particleOrigin(frac, random);

	float s, width, animationFrameFrac;
	particleTexCoords(frac, s, width, animationFrameFrac);

	float psize = parmEval(s_size, frac);
	float paspect = parmEval(s_aspect, frac);
	float quadWidth = psize;
	float quadHeight = psize * paspect;

	// the quads of animated particle are doubled: cross fade of two frames
	int animIndex = 0;
	int shapeIndex = quad;
	if (animationFrameFrac >= 0.0) {
		animIndex = quad & 1;
		shapeIndex = quad >> 1;
	}

	vec3 pos;
	vec2 st;
	float tc0y = 0.0, tc1y = 1.0;	// t of the upper and lower corners

	// corners: 0 1 / 2 3
	bool right = (corner & 1) != 0;
	bool lower = (corner & 2) != 0;

	if (s_orientation == POR_AIMED) {
		int numTrails = int(s_orientationParms[0]);
		float trailTime = s_orientationParms[1];
		if (trailTime == 0.0)
			trailTime = 0.5;

		float trailHeight = 1.0 / float(1 + numTrails);
		int i = shapeIndex;

		// origins: stepOrigin(i), oldOrigin(i), and the same for the previous trail (for its "left")
		vec3 forwardDir = entityAxisMul(s_viewAxisRow0);

		vec3 oldOrigins[3];		// oldOrigin of trails i-2, i-1, i
		for (int k = 0; k < 3; k++) {
			int trail = i - 2 + k;
			if (trail < 0) {
				oldOrigins[k] = origin;	// not used
				continue;
			}
			float ageMod = frac * s_particleLife - float(trail + 1) * trailTime / float(numTrails + 1);
			float fracMod = ageMod / s_particleLife;
			uint randomMod = seed;
			oldOrigins[k] = particleOrigin(fracMod, randomMod);
		}
		// origin of the beginning of trail j
		vec3 stepOrigin = (i == 0) ? origin : oldOrigins[1];
		vec3 oldOrigin = oldOrigins[2];

		vec3 up = stepOrigin - oldOrigin;
		up -= dot(up, forwardDir) * forwardDir;
		up = safeNormalize(up);
		vec3 left = cross(up, forwardDir) * psize;

		vec3 stepLeft = left;
		if (i > 0) {
			vec3 prevStepOrigin = (i == 1) ? origin : oldOrigins[0];
			vec3 prevOld = oldOrigins[1];
			vec3 prevUp = prevStepOrigin - prevOld;
			prevUp -= dot(prevUp, forwardDir) * forwardDir;
			prevUp = safeNormalize(prevUp);
			stepLeft = cross(prevUp, forwardDir) * psize;
		}

		vec3 base = lower ? oldOrigin : stepOrigin;
		vec3 offs = lower ? left : stepLeft;
		pos = right ? base + offs : base - offs;

		tc0y = float(i) * trailHeight;
		tc1y = tc0y + trailHeight;
	} else {
		// constant rotation
		float angle = (s_initialAngle != 0.0) ? s_initialAngle : 360.0 * rndFloat(random);

		float angleMove = parmIntegrate(s_rotationSpeed, frac) * s_particleLife;
		// half of the particles rotate each way
		if ((particle & 1) != 0) {
			angle += angleMove;
		} else {
			angle -= angleMove;
		}

		angle = angle * (PI / 180.0);
		float sn = sin(angle), cs = cos(angle);

		vec3 left, up;
		if (s_orientation == POR_Z) {
			left = vec3(sn, cs, 0.0);
			up = vec3(cs, -sn, 0.0);
		} else if (s_orientation == POR_X) {
			left = vec3(0.0, cs, sn);
			up = vec3(0.0, -sn, cs);
		} else if (s_orientation == POR_Y) {
			left = vec3(cs, 0.0, sn);
			up = vec3(-sn, 0.0, cs);
		} else {
			// oriented in viewer space
			vec3 entityLeft = entityAxisMul(s_viewAxisRow1);
			vec3 entityUp = entityAxisMul(s_viewAxisRow2);
			left = entityLeft * cs + entityUp * sn;
			up = entityUp * cs - entityLeft * sn;
		}

		left *= quadWidth;
		up *= quadHeight;

		pos = origin + (right ? left : -left) + (lower ? -up : up);
	}

	float sCorner = s + (right ? width : 0.0);
	if (animIndex == 1) {
		sCorner += 1.0 / float(s_animationFrames);
	}
	st = vec2(sCorner, lower ? tc1y : tc0y);

	if (animationFrameFrac >= 0.0) {
		color *= (animIndex == 0) ? (1.0 - animationFrameFrac) : animationFrameFrac;
	}

	writeVertex(pos, st, color);
}

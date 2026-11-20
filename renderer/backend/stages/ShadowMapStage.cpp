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
#include "renderer/backend/stages/ShadowMapStage.h"
#include "renderer/backend/RenderBackend.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/backend/GLSLUniforms.h"
#include "renderer/backend/GLSLProgramManager.h"

idCVarBool r_shadowMapCullFront(
	"r_shadowMapCullFront", "0", CVAR_ARCHIVE | CVAR_RENDERER,
	"Cull front faces in shadow maps"
);
idCVar r_shadowMapCullFrontSsaoLeakNear(
	"r_shadowMapCullFrontSsaoLeakNear", "300", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapContact: the contact shadows are fully applied within this distance of the player (world units),\n"
	"then fade out smoothly to nothing at r_shadowMapCullFrontSsaoLeakDist.",
	0.0f, 100000.0f
);
idCVar r_shadowMapCullFrontSsaoLeakDist(
	"r_shadowMapCullFrontSsaoLeakDist", "1200", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"Distance from the player (world units) at which r_shadowMapContact is fully faded out and skipped. 0 - no limit.",
	0.0f, 100000.0f
);
idCVar r_shadowMapContact(
	"r_shadowMapContact", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"Shadow maps: screen-space contact shadows. A ray is marched from every lit pixel towards the light through the depth buffer, and if a visible surface is found in the way, the pixel is shadowed.\n"
	"This corrects light leaks which the shadow map itself cannot (it can only add shadow). The value is the strength (0 = off). Fades out with the distance, see r_shadowMapCullFrontSsaoLeakNear / Dist.",
	0.0f, 1.0f
);
idCVar r_shadowMapContactDist(
	"r_shadowMapContactDist", "24", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"r_shadowMapContact: length of the ray in world units. Leaks are found only if the occluder is within this distance from the pixel.",
	2.0f, 200.0f
);
idCVar r_shadowMapContactBlur(
	"r_shadowMapContactBlur", "1.5", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"r_shadowMapContact: radius (world units) of the blur of the contact occlusion along the surface. Softens halo edges, costs 5 marches with half the steps each. 0 = off.",
	0.0f, 16.0f
);
idCVar r_shadowMapContactBlurDark(
	"r_shadowMapContactBlurDark", "2", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"r_shadowMapContactBlur: weight of the darkest blur sample against the average of all (weight 1). 2 = leans dark 2 to 1, 0 = plain average.",
	0.0f, 8.0f
);
idCVar r_shadowMapContactThickness(
	"r_shadowMapContactThickness", "1.5", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"r_shadowMapContact: assumed thickness (world units, grows with distance from camera) of any visible surface. A ray passing behind it by more than this is not blocked. Lower removes halos around small objects, higher seals thicker leaks.",
	0.2f, 16.0f
);
idCVar r_shadowMapContactCreaseMode(
	"r_shadowMapContactCreaseMode", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER,
	"r_shadowMapContact: where to march. 0 - everywhere, 1 - only in creases, measured from the depth buffer.",
	0, 1, idCmdSystem::ArgCompletion_Integer<0, 1>
);
idCVar r_shadowMapContactCreaseRadius(
	"r_shadowMapContactCreaseRadius", "4", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"r_shadowMapContactCreaseMode 1: radius in world units of the ring around the pixel which is searched for surfaces in front of it.",
	0.5f, 32.0f
);
idCVar r_shadowMapContactSteps(
	"r_shadowMapContactSteps", "10", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER,
	"r_shadowMapContact: number of depth buffer samples along the ray (cost per lit pixel).",
	2, 32
);
idCVarBool r_shadowMapContactSkipNoSelfShadow(
	"r_shadowMapContactSkipNoSelfShadow", "1", CVAR_ARCHIVE | CVAR_RENDERER,
	"r_shadowMapContact: do not apply the contact march to surfaces with noSelfShadow (they cannot be told apart as blockers, but are not darkened themselves)."
);
idCVar r_shadowMapContactWorldOnly(
	"r_shadowMapContactWorldOnly", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER,
	"r_shadowMapContact: restrict contact shadows (both blockers and receivers) to world/brush geometry so small props do not cast noisy halos.\n"
	"0 - all geometry, 1 - worldspawn and inline func_static brushes/patches, 2 - worldspawn only.",
	0, 2, idCmdSystem::ArgCompletion_Integer<0, 2>
);

bool R_EarlyDepthAvailable( const viewDef_t *viewDef ) {
	return !viewDef->IsLightGem() && !r_skipDepthCapture.GetBool()
		&& ( !viewDef->isSubview || viewDef->isXray || viewDef->isMirrorInverted )
		&& viewDef->renderWorld && !viewDef->renderWorld->mapName.IsEmpty();	// compass
}

bool R_ContactUsesWorldDepth( const viewDef_t *viewDef ) {
	// the extra depth copy is paid only if contact shadows can actually run: they exist only with cull-front shadow maps
	return r_shadowMapCullFront.GetBool() && r_shadowMapContact.GetFloat() > 0.0f
		&& r_shadowMapContactWorldOnly.GetInteger() > 0 && R_EarlyDepthAvailable( viewDef );
}
idCVar r_shadowMapCullFrontBiasSlope(
	"r_shadowMapCullFrontBiasSlope", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFront: slope-scaled bias factor for shadow map depth (emulates polygon offset factor, in linear distance).\n"
	"Positive moves stored surfaces towards the light (more shadow at contact), negative moves them away. 0 - off.",
	-8.0f, 8.0f
);
idCVar r_shadowMapCullFrontBiasClamp(
	"r_shadowMapCullFrontBiasClamp", "2", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFrontBiasSlope: maximum slope-dependent bias in world units (emulates glPolygonOffsetClamp).",
	0.0f, 64.0f
);
idCVar r_shadowMapCullFrontBiasUnits(
	"r_shadowMapCullFrontBiasUnits", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFront: constant bias of shadow map depth in world units. Positive moves stored surfaces towards the light. 0 - off.",
	-8.0f, 8.0f
);
idCVar r_shadowMapCullFrontExpand(
	"r_shadowMapCullFrontExpand", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFront: grow shadow casters by this many shadow map texels when they are rendered into shadow maps (uses geometry shader).\n"
	"This closes light leaks at seams of casters. It is paid at rendering of the shadow map, not for every lit pixel.\n"
	"Try 2-4. 0 - off.",
	0.0f, 8.0f
);
idCVar r_shadowMapCullFrontExpandDist(
	"r_shadowMapCullFrontExpandDist", "1200", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFrontExpand: only lights whose volume reaches within this distance of the player (world units) get expanded shadow casters.\n"
	"Light leaks of faraway lights are small on screen, and expansion has some cost. 0 - no limit.",
	0.0f, 100000.0f
);
idCVar r_shadowMapCullFrontExpandNear(
	"r_shadowMapCullFrontExpandNear", "300", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT,
	"With r_shadowMapCullFrontExpand and r_shadowMapCullFrontExpandDist: casters closer than this to the player (world units) are expanded fully,\n"
	"then the expansion fades out smoothly to nothing at r_shadowMapCullFrontExpandDist.",
	0.0f, 100000.0f
);
idCVar r_shadowMapCullFrontExpandWorldOnly(
	"r_shadowMapCullFrontExpandWorldOnly", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER,
	"With r_shadowMapCullFrontExpand: restrict caster expansion to world/brush geometry so small props and AI do not bloat.\n"
	"0 - all geometry, 1 - worldspawn and inline func_static brushes/patches, 2 - worldspawn only.",
	0, 2, idCmdSystem::ArgCompletion_Integer<0, 2>
);
idCVar r_shadowMapDebug(
	"r_shadowMapDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
	"Debug views of shadow map lighting (colors of lights are added, so use a single light):\n"
	"0 - off\n"
	"1 - final shadow coefficient\n"
	"2 - decision of the central sample: black = shadow, white = lit, magenta = lit because shadow map texel is empty, red = lit but within error margin\n"
	"5 - error margin in shadow map texels (white = 50)",
	0, 5, idCmdSystem::ArgCompletion_Integer<0, 5>
);
idCVar r_shadowMapSinglePass(
	"r_shadowMapSinglePass", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_BOOL,
	"1 - render shadow maps for all lights in a single pass"
);
idCVar r_shadowMapAlphaTested(
	"r_shadowMapAlphaTested", "1", CVAR_BOOL | CVAR_RENDERER | CVAR_ARCHIVE,
	"In case of alpha-tested material, apply alpha test to shadows too?\n"
	"Note: stencil shadows cannot work with alpha test."
);


struct ShadowMapStage::Uniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( Uniforms )
	DEFINE_UNIFORM( mat4, modelMatrix )
	DEFINE_UNIFORM( vec4, lightOrigin )
	DEFINE_UNIFORM( float, maxLightDistance )
	DEFINE_UNIFORM( float, alphaTest )
	DEFINE_UNIFORM( sampler, opaqueTexture )
	DEFINE_UNIFORM( mat4, textureMatrix )
	DEFINE_UNIFORM( vec3, depthBias )
	DEFINE_UNIFORM( vec2, expandNdc )
	DEFINE_UNIFORM( vec3, viewOrigin )
	DEFINE_UNIFORM( vec2, expandFalloff )
};

ShadowMapStage::ShadowMapStage() {}

void ShadowMapStage::Init() {
	shadowMapShader = programManager->LoadFromFiles( "shadow_map", "stages/shadow_map/shadow_map.vert.glsl", "stages/shadow_map/shadow_map.frag.glsl" );
	shadowMapBiasShader = programManager->LoadFromFiles( "shadow_map_bias", "stages/shadow_map/shadow_map.vert.glsl", "stages/shadow_map/shadow_map_bias.frag.glsl" );
	// variants with geometry shader which expands casters (r_shadowMapCullFrontExpand)
	idHashMapDict gsDefines;
	gsDefines.Set( "SHADOW_MAP_GS", "1" );
	shadowMapExpandShader = programManager->LoadFromFiles( "shadow_map_expand", "stages/shadow_map/shadow_map.vert.glsl", "stages/shadow_map/shadow_map.frag.glsl", "stages/shadow_map/shadow_map_expand.geom.glsl", gsDefines );
	shadowMapExpandBiasShader = programManager->LoadFromFiles( "shadow_map_expand_bias", "stages/shadow_map/shadow_map.vert.glsl", "stages/shadow_map/shadow_map_bias.frag.glsl", "stages/shadow_map/shadow_map_expand.geom.glsl", gsDefines );
}

void ShadowMapStage::Shutdown() {}

void ShadowMapStage::Start( const viewDef_t *viewDef ) {
	GL_CheckErrors();
	frameBuffers->EnterShadowMap();

	GL_Cull( r_shadowMapCullFront ? CT_BACK_SIDED : CT_TWO_SIDED );
	qglPolygonOffset( 0, 0 );
	qglEnable( GL_POLYGON_OFFSET_FILL );
	for ( int i = 0; i < 5; i++ ) {
		qglEnable( GL_CLIP_DISTANCE0 + i );
	}

	// the variant with depth bias writes gl_FragDepth, so it is used only when the bias is enabled
	float biasSlope = r_shadowMapCullFrontBiasSlope.GetFloat();
	float biasUnits = r_shadowMapCullFrontBiasUnits.GetFloat();
	bool useBias = r_shadowMapCullFront && ( biasSlope != 0.0f || biasUnits != 0.0f );
	// the variant with geometry shader grows casters, it is used only when expansion is enabled
	expandTexels = r_shadowMapCullFront ? r_shadowMapCullFrontExpand.GetFloat() : 0.0f;
	bool useExpand = expandTexels > 0.0f;
	// choose programs: plain one and (if enabled) the one with geometry shader, which is used only for lights close to the player
	biasParams = idVec3( biasSlope, r_shadowMapCullFrontBiasClamp.GetFloat(), biasUnits );
	usesBias = useBias;
	plainProgram = useBias ? shadowMapBiasShader : shadowMapShader;
	expandProgram = useExpand ? ( useBias ? shadowMapExpandBiasShader : shadowMapExpandShader ) : nullptr;
	expandDistance = r_shadowMapCullFrontExpandDist.GetFloat();
	expandWorldMode = r_shadowMapCullFrontExpandWorldOnly.GetInteger();
	viewOrigin = viewDef->renderView.vieworg;
	lightExpandAllowed = false;
	plainLightUniformsDirty = false;
	expandLightUniformsDirty = false;
	activeProgram = nullptr;
	SwitchProgram( plainProgram );
}

void ShadowMapStage::SwitchProgram( GLSLProgram *program ) {
	if ( program != activeProgram ) {
		activeProgram = program;
		program->Activate();
		uniforms = program->GetUniformGroup<Uniforms>();
		uniforms->opaqueTexture.Set( 0 );
		if ( usesBias )
			uniforms->depthBias.Set( biasParams.x, biasParams.y, biasParams.z );
	}
	if ( program == plainProgram && plainLightUniformsDirty ) {
		uniforms->lightOrigin.Set( currentLightOrigin );
		uniforms->maxLightDistance.Set( currentMaxLightDistance );
		plainLightUniformsDirty = false;
	} else if ( program == expandProgram && expandLightUniformsDirty ) {
		uniforms->lightOrigin.Set( currentLightOrigin );
		uniforms->maxLightDistance.Set( currentMaxLightDistance );
		uniforms->expandNdc.Set( currentExpandNdc );
		uniforms->viewOrigin.Set( viewOrigin );
		uniforms->expandFalloff.Set( currentExpandFalloff );
		expandLightUniformsDirty = false;
	}
}

bool ShadowMapStage::ShouldDrawLight( const viewLight_t *vLight, const DrawMask &mask ) {
	if ( vLight->noShadows ) {
		return false;
	}
	if ( mask.alpha ) {
		return vLight->shadows == LS_STENCIL && vLight->alphaShadowPage.width > 0 &&
			( mask.clear || mask.global && vLight->alphaShadows || mask.local && vLight->alphaLocalShadows );
	}
	if ( vLight->shadows != LS_MAPS) {
		return false;
	}
	if ( vLight->shadowMapPage.width == 0 ) {
		return false;
	}
	if ( !( mask.clear || mask.global && vLight->globalShadows || mask.local && vLight->localShadows ) ) {
		return false;
	}
	return true;
}

void ShadowMapStage::DrawLight( const viewLight_t *vLight, const DrawMask &mask ) {
	// expand casters only for lights which can affect what is near the player
	lightExpandAllowed = false;
	if ( expandProgram && !mask.alpha ) {
		lightExpandAllowed = true;
		if ( expandDistance > 0.0f ) {
			float distToLightVolume = ( vLight->globalLightOrigin - viewOrigin ).Length() - vLight->maxLightDistance;
			lightExpandAllowed = distToLightVolume < expandDistance;
		}
	}
	// thin alpha cards: front and back are the same depth, so cull-front would make them shadow themselves completely
	GL_Cull( mask.alpha ? CT_TWO_SIDED : ( r_shadowMapCullFront ? CT_BACK_SIDED : CT_TWO_SIDED ) );

	currentLightOrigin = idVec4( vLight->globalLightOrigin.x, vLight->globalLightOrigin.y, vLight->globalLightOrigin.z, 0.0f );
	currentMaxLightDistance = vLight->maxLightDistance;
	plainLightUniformsDirty = true;
	expandLightUniformsDirty = lightExpandAllowed;

	const renderCrop_t &page = mask.alpha ? vLight->alphaShadowPage : vLight->shadowMapPage;
	qglViewport( page.x, page.y, 6*page.width, page.width );
	// note: scissor must be always applied for correctness, also it is not view-dependent
	qglScissor( page.x, page.y, 6*page.width, page.width );
	if ( lightExpandAllowed ) {
		// the viewport is 6 faces wide, NDC range is 2 in both directions
		currentExpandNdc = idVec2( expandTexels * 2.0f / ( 6 * page.width ), expandTexels * 2.0f / page.width );
		float fadeEnd = expandDistance;
		float fadeStart = fadeEnd > 0.0f ? idMath::Fmin( r_shadowMapCullFrontExpandNear.GetFloat(), fadeEnd * 0.99f ) : 0.0f;
		currentExpandFalloff = idVec2( fadeStart, fadeEnd );
	}
	SwitchProgram( ( lightExpandAllowed && expandWorldMode <= 0 ) ? expandProgram : plainProgram );

	if ( mask.clear )
		qglClear( GL_DEPTH_BUFFER_BIT );

	if ( mask.global )
		DrawLightInteractions( mask.alpha ? vLight->alphaShadows : vLight->globalShadows );

	if ( mask.local )
		DrawLightInteractions( mask.alpha ? vLight->alphaLocalShadows : vLight->localShadows );
}

void ShadowMapStage::End() {
	for ( int i = 0; i < 5; i++ ) {
		qglDisable( GL_CLIP_DISTANCE0 + i );
	}

	qglDisable( GL_POLYGON_OFFSET_FILL );
	GL_Cull( CT_FRONT_SIDED );

	frameBuffers->LeaveShadowMap();
}

bool ShadowMapStage::DrawShadowMapSingleLight( const viewDef_t *viewDef, const viewLight_t *vLight, const DrawMask &mask ) {
	if ( !ShouldDrawLight( vLight, mask ) )
		return false;

	TRACE_GL_SCOPE( "DrawShadowMapSingleLight" );

	Start( viewDef );
	DrawLight( vLight, mask );
	End();

	return true;
}

void ShadowMapStage::DrawShadowMapAllLights( const viewDef_t *viewDef, const DrawMask &mask ) {
	TRACE_GL_SCOPE( "DrawShadowMapAllLights" );

	Start( viewDef );
	for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		if ( !ShouldDrawLight( vLight, mask ) )
			continue;
		DrawLight( vLight, mask );
	}
	End();
}

bool ShadowMapStage::ShouldDrawSurf( const drawSurf_t *surf ) const {
    const idMaterial *shader = surf->material;

    // some deforms may disable themselves by setting numIndexes = 0
    if ( !surf->numIndexes ) {
        return false;
    }

    if ( !surf->ambientCache.IsValid() || !surf->indexCache.IsValid() ) {
        #ifdef _DEBUG
        common->Printf( "ShadowMapStage: missing vertex or index cache\n" );
        #endif
        return false;
    }

	// #6571. parallax surfaces with this flag don't work well if the surface is rendered to shadow maps
	if ( auto parallaxStage = surf->material->GetParallaxStage() ) {
		if ( parallaxStage->parallax->offsetExternalShadows )
			return false;
	}

	return true;
}

void ShadowMapStage::DrawLightInteractions( const drawSurf_t *surfs ) {
	for ( const drawSurf_t *surf = surfs; surf; surf = surf->nextOnLight ) {
		if ( !ShouldDrawSurf( surf ) ) {
			continue;
		}
		DrawSurf( surf );
	}
}

void ShadowMapStage::DrawSurf( const drawSurf_t *surf ) {
	bool expandSurf = false;
	if ( lightExpandAllowed ) {
		if ( expandWorldMode <= 0 ) {
			expandSurf = true;
		} else {
			int worldMask = ( expandWorldMode >= 2 ) ? DSF_WORLD_GEOMETRY : ( DSF_WORLD_GEOMETRY | DSF_INLINE_MAP_MODEL );
			expandSurf = ( surf->dsFlags & worldMask ) != 0;
		}
	}
	SwitchProgram( expandSurf ? expandProgram : plainProgram );

	const idMaterial *shader = surf->material;

	vertexCache.VertexPosition( surf->ambientCache );

	bool drawSolid = false;

	if ( shader->Coverage() == MC_OPAQUE || shader->Coverage() == MC_TRANSLUCENT ) {
		drawSolid = true;
	}

	// we may have multiple alpha tested stages
	if ( shader->Coverage() == MC_PERFORATED ) {

		if ( r_shadowMapAlphaTested.GetBool() ) {
			// if the only alpha tested stages are condition register omitted,
			// draw a normal opaque surface
			bool	didDraw = false;

			GL_CheckErrors();

			// perforated surfaces may have multiple alpha tested stages
			for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
				const shaderStage_t *pStage = shader->GetStage( stage );

				if ( !pStage->hasAlphaTest ) {
					continue;
				}

				// check the stage enable condition
				if ( !surf->IsStageEnabled( pStage ) ) {
					continue;
				}

				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;

				// skip the entire stage if alpha would be black
				if ( surf->GetStageColor( pStage )[3] <= 0 ) {
					continue;
				}
				IssueDrawCommand( surf, pStage );
			}

			if ( !didDraw ) {
				drawSolid = true;
			}
		}
		else {
			drawSolid = true;
		}
	}

	if ( drawSolid ) {  // draw the entire surface solid
		IssueDrawCommand( surf, nullptr );
	}
}

void ShadowMapStage::IssueDrawCommand( const drawSurf_t *surf, const shaderStage_t *stage ) {
	if( stage ) {
		GL_SelectTexture( 0 );
		stage->texture.image->Bind();
	}

	uniforms->modelMatrix.Set( surf->space->modelMatrix );

	if( stage ) {
		// set the alpha modulate
		uniforms->alphaTest.Set( surf->shaderRegisters[stage->alphaTestRegister] );
		uniforms->textureMatrix.Set( surf->GetTextureMatrix( stage ) );
	}
	else {
		uniforms->alphaTest.Set( -1.0f );
	}

	RB_DrawElementsInstanced( surf, 6, DCK_SHADOW );
}

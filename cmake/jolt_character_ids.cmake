# SPDX-License-Identifier: Apache-2.0
# Reproducible narrow extension to pinned MIT Jolt sources. Original constructor
# remains available. The explicit-ID overload prevents failed live births from
# consuming Jolt's automatic allocator sequence. Never modify an unknown source.
function(poima_jolt_header_patch path)
    file(SHA256 "${path}" observed_sha)
    if(observed_sha STREQUAL "9dceba7e28d98d2589a007a3a7152916f855972d49a12bfbf21c62f6fda21ac0")
        return() # Idempotent; preserve source mtime and incremental build caches.
    endif()
    if(NOT observed_sha STREQUAL "efe3295fffc5581b0cb3dc97eed5cf33b4d5175fd7344f4c485d409190e93c98")
        message(FATAL_ERROR "Unsupported Jolt character source: ${path}; refusing automatic patch")
    endif()
    file(READ "${path}" content)
    string(REPLACE [=[										Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem);]=] [=[										Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem);

	/// Poima extension: reserve an explicit ID for transactional character ownership.
										Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem, const BodyID &inBodyID);]=] content "${content}")
    string(SHA256 patched "${content}")
    if(NOT patched STREQUAL "9dceba7e28d98d2589a007a3a7152916f855972d49a12bfbf21c62f6fda21ac0")
        message(FATAL_ERROR "Jolt character patch did not produce its reviewed source digest")
    endif()
    file(WRITE "${path}" "${content}")
endfunction()
function(poima_jolt_source_patch path)
    file(SHA256 "${path}" observed_sha)
    if(observed_sha STREQUAL "7cc3531d0fe30de331a4cf4edbe9b7a05032b88cd3772432adfecce4e4554b07")
        return() # Idempotent; preserve source mtime and incremental build caches.
    endif()
    if(NOT observed_sha STREQUAL "cb4f2c5d58fe778633e634798aa7d0d8a6b92ad8ac96f3f6c3de2f5f795c9b80")
        message(FATAL_ERROR "Unsupported Jolt character source: ${path}; refusing automatic patch")
    endif()
    file(READ "${path}" content)
    string(REPLACE [=[Character::Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem) :]=] [=[Character::Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem) :
	Character(inSettings, inPosition, inRotation, inUserData, inSystem, BodyID())
{
}

Character::Character(const CharacterSettings *inSettings, RVec3Arg inPosition, QuatArg inRotation, uint64 inUserData, PhysicsSystem *inSystem, const BodyID &inBodyID) :]=] content "${content}")
    string(REPLACE [=[const Body *body = mSystem->GetBodyInterface().CreateBody(settings);]=] [=[const Body *body = inBodyID.IsInvalid()? mSystem->GetBodyInterface().CreateBody(settings) : mSystem->GetBodyInterface().CreateBodyWithID(inBodyID, settings);]=] content "${content}")
    string(REPLACE [=[	mSystem->GetBodyInterface().DestroyBody(mBodyID);]=] [=[	if (!mBodyID.IsInvalid())
		mSystem->GetBodyInterface().DestroyBody(mBodyID);]=] content "${content}")
    string(SHA256 patched "${content}")
    if(NOT patched STREQUAL "7cc3531d0fe30de331a4cf4edbe9b7a05032b88cd3772432adfecce4e4554b07")
        message(FATAL_ERROR "Jolt character patch did not produce its reviewed source digest")
    endif()
    file(WRITE "${path}" "${content}")
endfunction()
function(poima_patch_jolt_character_ids source_directory)
    poima_jolt_header_patch("${source_directory}/Jolt/Physics/Character/Character.h")
    poima_jolt_source_patch("${source_directory}/Jolt/Physics/Character/Character.cpp")
endfunction()

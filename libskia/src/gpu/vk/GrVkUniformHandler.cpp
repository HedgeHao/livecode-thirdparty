/*
* Copyright 2016 Google Inc.
*
* Use of this source code is governed by a BSD-style license that can be
* found in the LICENSE file.
*/

#include "GrVkUniformHandler.h"
#include "glsl/GrGLSLProgramBuilder.h"

// To determine whether a current offset is aligned, we can just 'and' the lowest bits with the
// alignment mask. A value of 0 means aligned, any other value is how many bytes past alignment we
// are. This works since all alignments are powers of 2. The mask is always (alignment - 1).
// This alignment mask will give correct alignments for using the std430 block layout. If you want
// the std140 alignment, you can use this, but then make sure if you have an array type it is
// aligned to 16 bytes (i.e. has mask of 0xF).
uint32_t grsltype_to_alignment_mask(GrSLType type) {
    switch(type) {
        case kInt_GrSLType:
            return 0x3;
        case kUint_GrSLType:
            return 0x3;
        case kFloat_GrSLType:
            return 0x3;
        case kVec2f_GrSLType:
            return 0x7;
        case kVec3f_GrSLType:
            return 0xF;
        case kVec4f_GrSLType:
            return 0xF;
        case kMat22f_GrSLType:
            return 0x7;
        case kMat33f_GrSLType:
            return 0xF;
        case kMat44f_GrSLType:
            return 0xF;

        // This query is only valid for certain types.
        case kVoid_GrSLType:
        case kBool_GrSLType:
        case kTexture2DSampler_GrSLType:
        case kITexture2DSampler_GrSLType:
        case kTextureExternalSampler_GrSLType:
        case kTexture2DRectSampler_GrSLType:
        case kBufferSampler_GrSLType:
        case kTexture2D_GrSLType:
        case kSampler_GrSLType:
        case kImageStorage2D_GrSLType:
        case kIImageStorage2D_GrSLType:
            break;
    }
    SkFAIL("Unexpected type");
    return 0;
}

/** Returns the size in bytes taken up in vulkanbuffers for floating point GrSLTypes.
    For non floating point type returns 0. Currently this reflects the std140 alignment
    so a mat22 takes up 8 floats. */
static inline uint32_t grsltype_to_vk_size(GrSLType type) {
    switch(type) {
        case kInt_GrSLType:
            return 4;
        case kUint_GrSLType:
            return 4;
        case kFloat_GrSLType:
            return sizeof(float);
        case kVec2f_GrSLType:
            return 2 * sizeof(float);
        case kVec3f_GrSLType:
            return 3 * sizeof(float);
        case kVec4f_GrSLType:
            return 4 * sizeof(float);
        case kMat22f_GrSLType:
            //TODO: this will be 4 * szof(float) on std430.
            return 8 * sizeof(float);
        case kMat33f_GrSLType:
            return 12 * sizeof(float);
        case kMat44f_GrSLType:
            return 16 * sizeof(float);

        // This query is only valid for certain types.
        case kVoid_GrSLType:
        case kBool_GrSLType:
        case kTexture2DSampler_GrSLType:
        case kITexture2DSampler_GrSLType:
        case kTextureExternalSampler_GrSLType:
        case kTexture2DRectSampler_GrSLType:
        case kBufferSampler_GrSLType:
        case kTexture2D_GrSLType:
        case kSampler_GrSLType:
        case kImageStorage2D_GrSLType:
        case kIImageStorage2D_GrSLType:
            break;
    }
    SkFAIL("Unexpected type");
    return 0;
}


// Given the current offset into the ubo, calculate the offset for the uniform we're trying to add
// taking into consideration all alignment requirements. The uniformOffset is set to the offset for
// the new uniform, and currentOffset is updated to be the offset to the end of the new uniform.
void get_ubo_aligned_offset(uint32_t* uniformOffset,
                            uint32_t* currentOffset,
                            GrSLType type,
                            int arrayCount) {
    uint32_t alignmentMask = grsltype_to_alignment_mask(type);
    // We want to use the std140 layout here, so we must make arrays align to 16 bytes.
    if (arrayCount || type == kMat22f_GrSLType) {
        alignmentMask = 0xF;
    }
    uint32_t offsetDiff = *currentOffset & alignmentMask;
    if (offsetDiff != 0) {
        offsetDiff = alignmentMask - offsetDiff + 1;
    }
    *uniformOffset = *currentOffset + offsetDiff;
    SkASSERT(sizeof(float) == 4);
    if (arrayCount) {
        uint32_t elementSize = SkTMax<uint32_t>(16, grsltype_to_vk_size(type));
        SkASSERT(0 == (elementSize & 0xF));
        *currentOffset = *uniformOffset + elementSize * arrayCount;
    } else {
        *currentOffset = *uniformOffset + grsltype_to_vk_size(type);
    }
}

GrGLSLUniformHandler::UniformHandle GrVkUniformHandler::internalAddUniformArray(
                                                                            uint32_t visibility,
                                                                            GrSLType type,
                                                                            GrSLPrecision precision,
                                                                            const char* name,
                                                                            bool mangleName,
                                                                            int arrayCount,
                                                                            const char** outName) {
    SkASSERT(name && strlen(name));
    SkDEBUGCODE(static const uint32_t kVisibilityMask = kVertex_GrShaderFlag|kFragment_GrShaderFlag);
    SkASSERT(0 == (~kVisibilityMask & visibility));
    SkASSERT(0 != visibility);
    SkASSERT(kDefault_GrSLPrecision == precision || GrSLTypeIsFloatType(type));
    GrSLTypeIsFloatType(type);

    UniformInfo& uni = fUniforms.push_back();
    uni.fVariable.setType(type);
    // TODO this is a bit hacky, lets think of a better way.  Basically we need to be able to use
    // the uniform view matrix name in the GP, and the GP is immutable so it has to tell the PB
    // exactly what name it wants to use for the uniform view matrix.  If we prefix anythings, then
    // the names will mismatch.  I think the correct solution is to have all GPs which need the
    // uniform view matrix, they should upload the view matrix in their setData along with regular
    // uniforms.
    char prefix = 'u';
    if ('u' == name[0]) {
        prefix = '\0';
    }
    fProgramBuilder->nameVariable(uni.fVariable.accessName(), prefix, name, mangleName);
    uni.fVariable.setArrayCount(arrayCount);
    // For now asserting the the visibility is either only vertex or only fragment
    SkASSERT(kVertex_GrShaderFlag == visibility || kFragment_GrShaderFlag == visibility);
    uni.fVisibility = visibility;
    uni.fVariable.setPrecision(precision);
    // When outputing the GLSL, only the outer uniform block will get the Uniform modifier. Thus
    // we set the modifier to none for all uniforms declared inside the block.
    uni.fVariable.setTypeModifier(GrShaderVar::kNone_TypeModifier);

    uint32_t* currentOffset = kVertex_GrShaderFlag == visibility ? &fCurrentVertexUBOOffset
                                                                 : &fCurrentFragmentUBOOffset;
    get_ubo_aligned_offset(&uni.fUBOffset, currentOffset, type, arrayCount);

    if (outName) {
        *outName = uni.fVariable.c_str();
    }

    return GrGLSLUniformHandler::UniformHandle(fUniforms.count() - 1);
}

GrGLSLUniformHandler::SamplerHandle GrVkUniformHandler::addSampler(uint32_t visibility,
                                                                   GrSwizzle swizzle,
                                                                   GrSLType type,
                                                                   GrSLPrecision precision,
                                                                   const char* name) {
    SkASSERT(name && strlen(name));
    SkDEBUGCODE(static const uint32_t kVisMask = kVertex_GrShaderFlag | kFragment_GrShaderFlag);
    SkASSERT(0 == (~kVisMask & visibility));
    SkASSERT(0 != visibility);
    SkString mangleName;
    char prefix = 'u';
    fProgramBuilder->nameVariable(&mangleName, prefix, name, true);

    UniformInfo& info = fSamplers.push_back();
    SkASSERT(GrSLTypeIsCombinedSamplerType(type));
    info.fVariable.setType(type);
    info.fVariable.setTypeModifier(GrShaderVar::kUniform_TypeModifier);
    info.fVariable.setPrecision(precision);
    info.fVariable.setName(mangleName);
    SkString layoutQualifier;
    layoutQualifier.appendf("set=%d, binding=%d", kSamplerDescSet, fSamplers.count() - 1);
    info.fVariable.addLayoutQualifier(layoutQualifier.c_str());
    info.fVisibility = visibility;
    info.fUBOffset = 0;
    fSamplerSwizzles.push_back(swizzle);
    SkASSERT(fSamplerSwizzles.count() == fSamplers.count());
    return GrGLSLUniformHandler::SamplerHandle(fSamplers.count() - 1);
}

void GrVkUniformHandler::appendUniformDecls(GrShaderFlags visibility, SkString* out) const {
    SkASSERT(kVertex_GrShaderFlag == visibility || kFragment_GrShaderFlag == visibility);

    for (int i = 0; i < fSamplers.count(); ++i) {
        const UniformInfo& sampler = fSamplers[i];
        SkASSERT(sampler.fVariable.getType() == kTexture2DSampler_GrSLType);
        if (visibility == sampler.fVisibility) {
            sampler.fVariable.appendDecl(fProgramBuilder->shaderCaps(), out);
            out->append(";\n");
        }
    }

    SkString uniformsString;
    for (int i = 0; i < fUniforms.count(); ++i) {
        const UniformInfo& localUniform = fUniforms[i];
        if (visibility == localUniform.fVisibility) {
            if (GrSLTypeIsFloatType(localUniform.fVariable.getType())) {
                localUniform.fVariable.appendDecl(fProgramBuilder->shaderCaps(), &uniformsString);
                uniformsString.append(";\n");
            }
        }
    }
    if (!uniformsString.isEmpty()) {
        uint32_t uniformBinding = (visibility == kVertex_GrShaderFlag) ? kVertexBinding
                                                                       : kFragBinding;
        const char* stage = (visibility == kVertex_GrShaderFlag) ? "vertex" : "fragment";
        out->appendf("layout (set=%d, binding=%d) uniform %sUniformBuffer\n{\n",
                     kUniformBufferDescSet, uniformBinding, stage);
        out->appendf("%s\n};\n", uniformsString.c_str());
    }
}

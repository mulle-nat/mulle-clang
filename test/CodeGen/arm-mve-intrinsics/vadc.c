// NOTE: Assertions have been autogenerated by utils/update_cc_test_checks.py
// RUN: %clang_cc1 -triple thumbv8.1m.main-arm-none-eabi -target-feature +mve.fp -mfloat-abi hard -fallow-half-arguments-and-returns -O0 -disable-O0-optnone -S -emit-llvm -o - %s | opt -S -mem2reg | FileCheck %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-arm-none-eabi -target-feature +mve.fp -mfloat-abi hard -fallow-half-arguments-and-returns -O0 -disable-O0-optnone -DPOLYMORPHIC -S -emit-llvm -o - %s | opt -S -mem2reg | FileCheck %s

#include <arm_mve.h>

// CHECK-LABEL: @test_vadciq_s32(
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[TMP0:%.*]] = call { <4 x i32>, i32 } @llvm.arm.mve.vadc.v4i32(<4 x i32> [[A:%.*]], <4 x i32> [[B:%.*]], i32 0)
// CHECK-NEXT:    [[TMP1:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP0]], 1
// CHECK-NEXT:    [[TMP2:%.*]] = lshr i32 [[TMP1]], 29
// CHECK-NEXT:    [[TMP3:%.*]] = and i32 1, [[TMP2]]
// CHECK-NEXT:    store i32 [[TMP3]], i32* [[CARRY_OUT:%.*]], align 4
// CHECK-NEXT:    [[TMP4:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP0]], 0
// CHECK-NEXT:    ret <4 x i32> [[TMP4]]
//
int32x4_t test_vadciq_s32(int32x4_t a, int32x4_t b, unsigned *carry_out)
{
#ifdef POLYMORPHIC
    return vadciq(a, b, carry_out);
#else /* POLYMORPHIC */
    return vadciq_s32(a, b, carry_out);
#endif /* POLYMORPHIC */
}

// CHECK-LABEL: @test_vadcq_u32(
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[TMP0:%.*]] = load i32, i32* [[CARRY:%.*]], align 4
// CHECK-NEXT:    [[TMP1:%.*]] = shl i32 [[TMP0]], 29
// CHECK-NEXT:    [[TMP2:%.*]] = call { <4 x i32>, i32 } @llvm.arm.mve.vadc.v4i32(<4 x i32> [[A:%.*]], <4 x i32> [[B:%.*]], i32 [[TMP1]])
// CHECK-NEXT:    [[TMP3:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP2]], 1
// CHECK-NEXT:    [[TMP4:%.*]] = lshr i32 [[TMP3]], 29
// CHECK-NEXT:    [[TMP5:%.*]] = and i32 1, [[TMP4]]
// CHECK-NEXT:    store i32 [[TMP5]], i32* [[CARRY]], align 4
// CHECK-NEXT:    [[TMP6:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP2]], 0
// CHECK-NEXT:    ret <4 x i32> [[TMP6]]
//
uint32x4_t test_vadcq_u32(uint32x4_t a, uint32x4_t b, unsigned *carry)
{
#ifdef POLYMORPHIC
    return vadcq(a, b, carry);
#else /* POLYMORPHIC */
    return vadcq_u32(a, b, carry);
#endif /* POLYMORPHIC */
}

// CHECK-LABEL: @test_vadciq_m_u32(
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[TMP0:%.*]] = zext i16 [[P:%.*]] to i32
// CHECK-NEXT:    [[TMP1:%.*]] = call <4 x i1> @llvm.arm.mve.pred.i2v.v4i1(i32 [[TMP0]])
// CHECK-NEXT:    [[TMP2:%.*]] = call { <4 x i32>, i32 } @llvm.arm.mve.vadc.predicated.v4i32.v4i1(<4 x i32> [[INACTIVE:%.*]], <4 x i32> [[A:%.*]], <4 x i32> [[B:%.*]], i32 0, <4 x i1> [[TMP1]])
// CHECK-NEXT:    [[TMP3:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP2]], 1
// CHECK-NEXT:    [[TMP4:%.*]] = lshr i32 [[TMP3]], 29
// CHECK-NEXT:    [[TMP5:%.*]] = and i32 1, [[TMP4]]
// CHECK-NEXT:    store i32 [[TMP5]], i32* [[CARRY_OUT:%.*]], align 4
// CHECK-NEXT:    [[TMP6:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP2]], 0
// CHECK-NEXT:    ret <4 x i32> [[TMP6]]
//
uint32x4_t test_vadciq_m_u32(uint32x4_t inactive, uint32x4_t a, uint32x4_t b, unsigned *carry_out, mve_pred16_t p)
{
#ifdef POLYMORPHIC
    return vadciq_m(inactive, a, b, carry_out, p);
#else /* POLYMORPHIC */
    return vadciq_m_u32(inactive, a, b, carry_out, p);
#endif /* POLYMORPHIC */
}

// CHECK-LABEL: @test_vadcq_m_s32(
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[TMP0:%.*]] = load i32, i32* [[CARRY:%.*]], align 4
// CHECK-NEXT:    [[TMP1:%.*]] = shl i32 [[TMP0]], 29
// CHECK-NEXT:    [[TMP2:%.*]] = zext i16 [[P:%.*]] to i32
// CHECK-NEXT:    [[TMP3:%.*]] = call <4 x i1> @llvm.arm.mve.pred.i2v.v4i1(i32 [[TMP2]])
// CHECK-NEXT:    [[TMP4:%.*]] = call { <4 x i32>, i32 } @llvm.arm.mve.vadc.predicated.v4i32.v4i1(<4 x i32> [[INACTIVE:%.*]], <4 x i32> [[A:%.*]], <4 x i32> [[B:%.*]], i32 [[TMP1]], <4 x i1> [[TMP3]])
// CHECK-NEXT:    [[TMP5:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP4]], 1
// CHECK-NEXT:    [[TMP6:%.*]] = lshr i32 [[TMP5]], 29
// CHECK-NEXT:    [[TMP7:%.*]] = and i32 1, [[TMP6]]
// CHECK-NEXT:    store i32 [[TMP7]], i32* [[CARRY]], align 4
// CHECK-NEXT:    [[TMP8:%.*]] = extractvalue { <4 x i32>, i32 } [[TMP4]], 0
// CHECK-NEXT:    ret <4 x i32> [[TMP8]]
//
int32x4_t test_vadcq_m_s32(int32x4_t inactive, int32x4_t a, int32x4_t b, unsigned *carry, mve_pred16_t p)
{
#ifdef POLYMORPHIC
    return vadcq_m(inactive, a, b, carry, p);
#else /* POLYMORPHIC */
    return vadcq_m_s32(inactive, a, b, carry, p);
#endif /* POLYMORPHIC */
}

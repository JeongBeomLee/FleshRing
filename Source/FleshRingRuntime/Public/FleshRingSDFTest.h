// FleshRingSDFTest.h
// CS Dispatch 테스트용 Blueprint Function Library
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FleshRingSDFTest.generated.h"

UCLASS()
class FLESHRINGRUNTIME_API UFleshRingSDFTest : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // Blueprint에서 호출 가능한 테스트 함수
    // 구체 SDF를 생성하고 결과를 로그로 출력
    UFUNCTION(BlueprintCallable, Category = "FleshRing|SDF Test")
    static void TestSphereSDF();
};

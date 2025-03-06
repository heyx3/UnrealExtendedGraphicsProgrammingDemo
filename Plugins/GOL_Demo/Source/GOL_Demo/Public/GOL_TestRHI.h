#pragma once

#include "CoreMinimal.h"

#include "GOL_TestRHI.generated.h"


UCLASS(BlueprintType)
class GOL_DEMO_API A_GOL_TestRHI : public AActor
{
	GENERATED_BODY()
public:
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient)
	UTextureRenderTarget2D* TestOutput = nullptr;
	
	A_GOL_TestRHI() { PrimaryActorTick.bCanEverTick = true; }
	
	virtual void BeginPlay() override;
	virtual void Tick(float deltaSeconds) override;
};
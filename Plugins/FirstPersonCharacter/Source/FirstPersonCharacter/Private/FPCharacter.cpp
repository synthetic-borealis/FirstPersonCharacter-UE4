// Copyright Ali El Saleh, 2020

#include "FPCharacter.h"
#include "FirstPersonFootstepData.h"

#include "Components/InputComponent.h"
#include "Components/CapsuleComponent.h"

#include "Camera/CameraComponent.h"

#include "GameFramework/Controller.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/SpringArmComponent.h"

#include "Kismet/GameplayStatics.h"

#include "Sound/SoundBase.h"

#include "GameplayCameras/Public/MatineeCameraShake.h"

AFPCharacter::AFPCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	SpringArmComponent = CreateDefaultSubobject<USpringArmComponent>(FName("SpringArmComponent"));
	SpringArmComponent->TargetArmLength = 0.0f;
	SpringArmComponent->SetupAttachment(GetCapsuleComponent());
	
	CameraComponent = CreateDefaultSubobject<UCameraComponent>(FName("CameraComponent"));
	CameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 70.0f));
	CameraComponent->bUsePawnControlRotation = true;
	CameraComponent->SetupAttachment(SpringArmComponent);

	// Other settings
	GetCharacterMovement()->MaxWalkSpeed = 300.0f;
	GetCharacterMovement()->JumpZVelocity = 300.0f;
	GetCharacterMovement()->AirControl = 0.1f;
	GetCapsuleComponent()->bReturnMaterialOnMove = true;
	
	AutoPossessPlayer = EAutoReceiveInput::Player0;
	AutoReceiveInput = EAutoReceiveInput::Player0;

	CrouchPhase = ECrouchPhase::Standing;
}

void AFPCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Get access to the input settings
	Input = const_cast<UInputSettings*>(GetDefault<UInputSettings>());

	// Movement setup
	CurrentWalkSpeed = Movement.WalkSpeed;
	GetCharacterMovement()->MaxWalkSpeed = CurrentWalkSpeed;
	GetCharacterMovement()->JumpZVelocity = Movement.JumpVelocity;
	
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (CameraManager)
	{
		CameraManager->ViewPitchMin = Camera.MinPitch;
		CameraManager->ViewPitchMax = Camera.MaxPitch;
	}

	// Initialization
	OriginalCameraLocation = CameraComponent->GetRelativeLocation();
	OriginalCapsuleHalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// Footstep setup
	LastLocation = GetActorLocation();
	LastFootstepLocation = GetActorLocation();
	TravelDistance = 0;

	// Input setup
	SetupInputBindings();
}

void AFPCharacter::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateCameraShake();

	UpdateCrouch(DeltaTime);
	UpdateWalkingSpeed();
}

void AFPCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Axis bindings
	PlayerInputComponent->BindAxis(FName("MoveForward"), this, &AFPCharacter::MoveForward);
	PlayerInputComponent->BindAxis(FName("MoveRight"), this, &AFPCharacter::MoveRight);
	PlayerInputComponent->BindAxis(FName("Turn"), this, &AFPCharacter::AddControllerYawInput);
	PlayerInputComponent->BindAxis(FName("LookUp"), this, &AFPCharacter::AddControllerPitchInput);

	// Action bindings
	PlayerInputComponent->BindAction(FName("Jump"), IE_Pressed, this, &AFPCharacter::Jump);
	PlayerInputComponent->BindAction(FName("Jump"), IE_Released, this, &AFPCharacter::StopJumping);
	PlayerInputComponent->BindAction(FName("Run"), IE_Pressed, this, &AFPCharacter::Run);
	PlayerInputComponent->BindAction(FName("Run"), IE_Released, this, &AFPCharacter::StopRunning);
	PlayerInputComponent->BindAction(FName("Crouch"), IE_Pressed, this, &AFPCharacter::StartCrouch);
	PlayerInputComponent->BindAction(FName("Crouch"), IE_Released, this, &AFPCharacter::StopCrouching);
	PlayerInputComponent->BindAction(FName("Interact"), IE_Pressed, this, &AFPCharacter::Interact);
	PlayerInputComponent->BindAction(FName("Escape"), IE_Pressed, this, &AFPCharacter::Quit);
}

void AFPCharacter::Jump()
{
	if (CrouchPhase == ECrouchPhase::Standing)
	{
		Super::Jump();

		// Play jump camera shake
		PlayerController->ClientStartCameraShake(CameraShakes.JumpShake);
	}
}

void AFPCharacter::Landed(const FHitResult& Hit)
{
	if (CrouchPhase == ECrouchPhase::Standing)
	{
		Super::Landed(Hit);

		// Play jump camera shake
		PlayerController->ClientStartCameraShake(CameraShakes.JumpShake, 3.0f);

		if (FootstepSettings.bEnableFootsteps)
			PlayFootstepSound();
	}
}

void AFPCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	PlayerController = Cast<APlayerController>(NewController);
}

void AFPCharacter::StartCrouch()
{
	switch (Movement.CrouchActionType)
	{
		case EPlayerActionType::Hold:
		{
			bWantsToCrouch = true;
			CrouchPhase = ECrouchPhase::InTransition;
			break;
		}
		
		case EPlayerActionType::Toggle:
		{
			if (bWantsToCrouch && !IsBlockedInCrouchStance())
			{
				bWantsToCrouch = false;
				CrouchPhase = ECrouchPhase::InTransition;
			}
			else if(!bWantsToCrouch)
			{
				bWantsToCrouch = true;
				CrouchPhase = ECrouchPhase::InTransition;
			}
			break;
		}
	}
}

void AFPCharacter::StopCrouching()
{
	if (Movement.CrouchActionType == EPlayerActionType::Hold)
	{
		bWantsToCrouch = false;
		CrouchPhase = ECrouchPhase::InTransition;
	}
}

void AFPCharacter::MoveForward(const float AxisValue)
{
	if (Controller)
	{
		FRotator ForwardRotation = Controller->GetControlRotation();

		// Limit pitch rotation
		if (GetCharacterMovement()->IsMovingOnGround() || GetCharacterMovement()->IsFalling())
			ForwardRotation.Pitch = 0.0f;

		// Find out which way is forward
		const FVector Direction = FRotationMatrix(ForwardRotation).GetScaledAxis(EAxis::X);

		// Apply movement in the calculated direction
		AddMovementInput(Direction, AxisValue);

		if (FootstepSettings.bEnableFootsteps)
		{
			// Continously add to Travel Distance when moving
			if (GetCharacterMovement()->Velocity.Size() > 0.0f && GetCharacterMovement()->IsMovingOnGround())
			{
				TravelDistance += (GetActorLocation() - LastLocation).Size();
				LastLocation = GetActorLocation();
			}
			// Reset when not moving AND if we are falling
			else if (GetCharacterMovement()->IsFalling())
			{
				LastLocation = GetActorLocation();
				TravelDistance = 0.0f;
			}

			// Is it time to play a footstep sound?
			if (GetCharacterMovement()->IsMovingOnGround() && TravelDistance > FootstepSettings.CurrentStride)
			{
				PlayFootstepSound();
				TravelDistance = 0;
			}
		}
	}
}

void AFPCharacter::MoveRight(const float AxisValue)
{
	if (Controller)
	{
		// Find out which way is right
		const FRotator RightRotation = Controller->GetControlRotation();
		const FVector Direction = FRotationMatrix(RightRotation).GetScaledAxis(EAxis::Y);

		// Apply movement in the calculated direction
		AddMovementInput(Direction, AxisValue);
	}
}

void AFPCharacter::Run()
{
	bWantsToRun = true;
}

void AFPCharacter::StopRunning()
{
	bWantsToRun = false;
}

void AFPCharacter::UpdateWalkingSpeed()
{
	if (CrouchPhase == ECrouchPhase::Standing)
	{
		CurrentWalkSpeed = bWantsToRun ? Movement.RunSpeed : Movement.WalkSpeed;
		GetCharacterMovement()->MaxWalkSpeed = CurrentWalkSpeed;
	}
}

void AFPCharacter::UpdateCrouch(const float DeltaTime)
{
	if (CrouchPhase == ECrouchPhase::InTransition)
	{
		const float ErrorMargin = 2.0f;
		if (bWantsToCrouch)
		{
			// Smoothly move camera to target location and smoothly decrease the capsule height to fit through small openings
			const FVector NewLocation = FMath::Lerp(CameraComponent->GetRelativeLocation(), FVector(0.0f, 0.0f, 30.0f), Movement.StandToCrouchTransitionSpeed * DeltaTime);
			const float NewHalfHeight = FMath::Lerp(GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), OriginalCapsuleHalfHeight/2.0f, Movement.StandToCrouchTransitionSpeed * DeltaTime);
			const float NewWalkSpeed = FMath::Lerp(GetCharacterMovement()->MaxWalkSpeed, Movement.CrouchSpeed, Movement.StandToCrouchTransitionSpeed * DeltaTime);
			

			// Change CrouchPhase when we reach the target
			if (FMath::IsNearlyEqual(NewHalfHeight, OriginalCapsuleHalfHeight / 2.0f, ErrorMargin))
			{
				CameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 30.0f));
				GetCapsuleComponent()->SetCapsuleHalfHeight(OriginalCapsuleHalfHeight / 2.0f);
				GetCharacterMovement()->MaxWalkSpeed = Movement.CrouchSpeed;
				CrouchPhase = ECrouchPhase::Crouching;
			}
			else
			{
				CameraComponent->SetRelativeLocation(NewLocation);
				GetCapsuleComponent()->SetCapsuleHalfHeight(NewHalfHeight);
				GetCharacterMovement()->MaxWalkSpeed = NewWalkSpeed;
			}
		}
		else if (!((Movement.CrouchActionType == EPlayerActionType::Hold) && IsBlockedInCrouchStance()))
		{
			// Smoothly move camera back to original location and smoothly increase the capsule height to the original height
			const FVector NewLocation = FMath::Lerp(CameraComponent->GetRelativeLocation(), OriginalCameraLocation, Movement.StandToCrouchTransitionSpeed * DeltaTime);
			const float NewHalfHeight = FMath::Lerp(GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), OriginalCapsuleHalfHeight, Movement.StandToCrouchTransitionSpeed * DeltaTime);
			const float NewWalkSpeed = FMath::Lerp(GetCharacterMovement()->MaxWalkSpeed, CurrentWalkSpeed, Movement.StandToCrouchTransitionSpeed * DeltaTime);

			// Change CrouchPhase when we reach the target
			if (FMath::IsNearlyEqual(NewHalfHeight, OriginalCapsuleHalfHeight, ErrorMargin))
			{
				CameraComponent->SetRelativeLocation(OriginalCameraLocation);
				GetCapsuleComponent()->SetCapsuleHalfHeight(OriginalCapsuleHalfHeight);
				CrouchPhase = ECrouchPhase::Standing;
				GetCharacterMovement()->MaxWalkSpeed = CurrentWalkSpeed;
			}
			else
			{
				CameraComponent->SetRelativeLocation(NewLocation);
				GetCapsuleComponent()->SetCapsuleHalfHeight(NewHalfHeight);
				GetCharacterMovement()->MaxWalkSpeed = NewWalkSpeed;
			}
		}
	}
}

bool AFPCharacter::IsBlockedInCrouchStance()
{
	// Cast a sphere abouve the character
	const FVector StartLocation = GetActorLocation();
	const float CurrentHalfHeight = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight_WithoutHemisphere();
	const float TraceDistance = Movement.CrouchActionType == EPlayerActionType::Hold
		? CurrentHalfHeight + Movement.BlockTestOffset
		: OriginalCapsuleHalfHeight;
	const FVector EndLocation = StartLocation + TraceDistance * GetActorUpVector();
	const FCollisionShape CollisionSphere
		= FCollisionShape::MakeSphere(GetCapsuleComponent()->GetUnscaledCapsuleRadius());
	
	FHitResult HitResult;
	FCollisionQueryParams SphereParams(SCENE_QUERY_STAT(CrouchTrace), false, this);
	FCollisionResponseParams ResponseParams;
	GetCharacterMovement()->InitCollisionParams(SphereParams, ResponseParams);

	bool bHasHit = GetWorld()->SweepSingleByChannel(
		HitResult,
		StartLocation,
		EndLocation,
		FQuat::Identity,
		ECC_Visibility,
		CollisionSphere,
		SphereParams,
		ResponseParams
	);
	
	if (bHasHit)
	{
		// Return true only if the hit actor doesn't simulate physics
		USceneComponent* HitComponent = HitResult.GetComponent();
		bool bOtherSimulatesPhysics = HitComponent->IsSimulatingPhysics();
		return !bOtherSimulatesPhysics;
	}
	else
	{
		return false;
	}
}

void AFPCharacter::UpdateCameraShake()
{	
	if (PlayerController)
	{
		// Shake camera (Walking shake)
		if (GetVelocity().Size() > 0 && CanJump())
			PlayerController->ClientStartCameraShake(CameraShakes.WalkShake, 2.0f);
		// Shake camera (breathing shake)
		else
			PlayerController->ClientStartCameraShake(CameraShakes.IdleShake, 1.0f);
		
		// Shake camera (Run shake)
		if (GetVelocity().Size() > 0 && GetCharacterMovement()->MaxWalkSpeed >= Movement.RunSpeed && CanJump())
			PlayerController->ClientStartCameraShake(CameraShakes.RunShake, 1.0f);
	}
}

void AFPCharacter::Quit()
{
	UKismetSystemLibrary::QuitGame(GetWorld(), Cast<APlayerController>(GetController()), EQuitPreference::Quit, true);
}

void AFPCharacter::Interact()
{
	UE_LOG(LogTemp, Warning, TEXT("No functionality, derive from this character and implement this event"))
}

void AFPCharacter::PlayFootstepSound()
{
	GetCharacterMovement()->FindFloor(GetCapsuleComponent()->GetComponentLocation(), FloorResult, false);

	if (FloorResult.bBlockingHit)
	{
		if (IsValid(GetFootstepSound(&FloorResult.HitResult.PhysMaterial)))
		{
			if (CrouchPhase != ECrouchPhase::Standing)
				UGameplayStatics::PlaySoundAtLocation(this, GetFootstepSound(&FloorResult.HitResult.PhysMaterial), FloorResult.HitResult.Location, 0.35f);
			else
				UGameplayStatics::PlaySoundAtLocation(this, GetFootstepSound(&FloorResult.HitResult.PhysMaterial), FloorResult.HitResult.Location);
		}
		else
		{
			AActor* FloorActor = FloorResult.HitResult.GetActor();
			if (FloorActor)
				UE_LOG(LogTemp, Warning, TEXT("No physical material found for %s"), *FloorActor->GetName())
		}
	}

	LastFootstepLocation = FloorResult.HitResult.Location;
}

USoundBase* AFPCharacter::GetFootstepSound(TWeakObjectPtr<UPhysicalMaterial>* Surface)
{
	for (auto FootstepMapping : FootstepSettings.Mappings)
	{
		if (FootstepMapping && FootstepMapping->GetPhysicalMaterial() == Surface->Get())
		{
			CurrentFootstepMapping = FootstepMapping;
			FootstepSettings.CurrentStride = (CrouchPhase != ECrouchPhase::Standing) ? FootstepMapping->GetFootstepStride_Crouch() : bWantsToRun ? FootstepMapping->GetFootstepStride_Run() : FootstepMapping->GetFootstepStride_Walk();
			return FootstepMapping->GetFootstepSounds()[FMath::RandRange(0, FootstepMapping->GetFootstepSounds().Num() - 1)];
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("No footstep sound"))
	return nullptr;
}

void AFPCharacter::SetupInputBindings()
{
	ActionMappings = Input->GetActionMappings();
	AxisMappings = Input->GetAxisMappings();

	// Exit early if we already have inputs set
	if (ActionMappings.Num() > 0 || AxisMappings.Num() > 0)
	{
		if (!bUseCustomKeyMappings)
			ResetToDefaultInputBindings();

		return;
	}
	
	ResetToDefaultInputBindings();
}

void AFPCharacter::ResetInputBindings()
{
	for (const auto& Action : ActionMappings)
		Input->RemoveActionMapping(Action);

	for (const auto& Axis : AxisMappings)
		Input->RemoveAxisMapping(Axis);
}

void AFPCharacter::ResetToDefaultInputBindings()
{
	// Clear all the action and axis mappings
	ResetInputBindings();

	// Populate the map with action and axis mappings
	TArray<FName> ActionNames;
	TArray<FName> AxisNames;
	TArray<FKey> ActionKeys;
	TArray<FKey> AxisKeys;
	TMap<FKey, FName> ActionMap;
	TMap<FKey, FName> AxisMap;
	ActionMap.Add(EKeys::SpaceBar, FName("Jump"));
	ActionMap.Add(EKeys::F, FName("Interact"));
	ActionMap.Add(EKeys::Escape, FName("Escape"));
	ActionMap.Add(EKeys::LeftShift, FName("Run"));
	ActionMap.Add(EKeys::LeftControl, FName("Crouch"));
	ActionMap.Add(EKeys::C, FName("Crouch"));
	AxisMap.Add(EKeys::MouseX, FName("Turn"));
	AxisMap.Add(EKeys::MouseY, FName("LookUp"));
	AxisMap.Add(EKeys::W, FName("MoveForward"));
	AxisMap.Add(EKeys::S, FName("MoveForward"));
	AxisMap.Add(EKeys::A, FName("MoveRight"));
	AxisMap.Add(EKeys::D, FName("MoveRight"));

	// Lambda function to set the new action mappings
	const auto SetActionMapping = [&](const FName Name, const FKey Key)
	{
		FInputActionKeyMapping ActionMapping;
		ActionMapping.ActionName = Name;
		ActionMapping.Key = bUseCustomKeyMappings ? EKeys::NAME_KeyboardCategory : Key;
		Input->AddActionMapping(ActionMapping);
	};

	// Lambda function to set the new axis mappings
	const auto SetAxisMapping = [&](const FName Name, const FKey Key, const float Scale)
	{
		FInputAxisKeyMapping AxisMapping;
		AxisMapping.AxisName = Name;
		AxisMapping.Key = bUseCustomKeyMappings ? EKeys::NAME_KeyboardCategory : Key;
		AxisMapping.Scale = Scale;
		Input->AddAxisMapping(AxisMapping);
	};

	// Loop through the entire ActionMap and assign action inputs
	ActionMap.GenerateKeyArray(ActionKeys);
	ActionMap.GenerateValueArray(ActionNames);
	for (int32 i = 0; i < ActionMap.Num(); i++)
	{
		SetActionMapping(ActionNames[i], ActionKeys[i]);
	}

	// Loop through the entire AxisMap and assign axis inputs
	AxisMap.GenerateKeyArray(AxisKeys);
	AxisMap.GenerateValueArray(AxisNames);
	for (int32 i = 0; i < AxisMap.Num(); i++)
	{
		const bool bIsNegativeScale = AxisKeys[i] == EKeys::S || AxisKeys[i] == EKeys::A || AxisKeys[i] == EKeys::MouseY;

		SetAxisMapping(AxisNames[i], AxisKeys[i], bIsNegativeScale ? -1.0f : 1.0f);
	}

	// Save to input config file
	Input->SaveKeyMappings();

	// Update in Project Settings -> Engine -> Input
	Input->ForceRebuildKeymaps();
}

void AFPCharacter::AddControllerYawInput(const float Value)
{
	return Super::AddControllerYawInput(Value * Camera.SensitivityX * GetWorld()->GetDeltaSeconds());
}

void AFPCharacter::AddControllerPitchInput(const float Value)
{
	Super::AddControllerPitchInput(Value * Camera.SensitivityY * GetWorld()->GetDeltaSeconds());
}

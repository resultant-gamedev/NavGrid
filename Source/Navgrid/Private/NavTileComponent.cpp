// Fill out your copyright notice in the Description page of Project Settings.

#include "NavGridPrivatePCH.h"
#include <limits>

UNavTileComponent::UNavTileComponent(const FObjectInitializer &ObjectInitializer)
	:Super(ObjectInitializer)
{
	SetComponentTickEnabled(false);
	bUseAttachParentBound = true;

	Extent = ObjectInitializer.CreateDefaultSubobject<UBoxComponent>(this, "Extent");
	Extent->SetupAttachment(this);
	Extent->SetBoxExtent(FVector(100, 100, 5));
	Extent->ShapeColor = FColor::Magenta;
	
	Extent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Extent->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block); // So we get mouse over events
	Extent->SetCollisionResponseToChannel(ANavGrid::ECC_Walkable, ECollisionResponse::ECR_Block); // So we can find the floor with a line trace
	Extent->OnBeginCursorOver.AddDynamic(this, &UNavTileComponent::CursorOver);
	Extent->OnEndCursorOver.AddDynamic(this, &UNavTileComponent::EndCursorOver);
	Extent->OnClicked.AddDynamic(this, &UNavTileComponent::Clicked);

	PawnLocationOffset = CreateDefaultSubobject<USceneComponent>(TEXT("PawnLocationOffset"));
	PawnLocationOffset->SetRelativeLocation(FVector::ZeroVector);
	PawnLocationOffset->SetupAttachment(this);
	PawnLocationOffset->SetVisibility(false);

	HoverCursor = ObjectInitializer.CreateDefaultSubobject<UStaticMeshComponent>(this, "HoverCursor");
	HoverCursor->SetupAttachment(PawnLocationOffset);
	HoverCursor->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	HoverCursor->ToggleVisibility(false);
	HoverCursor->SetRelativeLocation(FVector(0, 0, 50));
	HoverCursor->bUseAttachParentBound = true;
	auto HCRef = TEXT("StaticMesh'/NavGrid/SMesh/NavGrid_Cursor.NavGrid_Cursor'");
	auto HCFinder = ConstructorHelpers::FObjectFinder<UStaticMesh>(HCRef);
	if (HCFinder.Succeeded()) 
	{ 
		HoverCursor->SetStaticMesh(HCFinder.Object);
	}
	else
	{
		UE_LOG(NavGrid, Error, TEXT("Error loading %s"), HCRef);
	}

	LineBatchComponent = ObjectInitializer.CreateDefaultSubobject<ULineBatchComponent>(this, "LineBatchComponent");
	LineBatchComponent->SetupAttachment(this);

	TArray<USceneComponent *> Components;
	GetChildrenComponents(true, Components);
	for (USceneComponent *Comp : Components)
	{
		Comp->SetComponentTickEnabled(false);
		Comp->bUseAttachParentBound = true;
	}
}

void UNavTileComponent::BeginPlay()
{
	if (!Grid)
	{
		Grid = ANavGrid::GetNavGrid(GetWorld());
		if (!Grid)
		{
			UE_LOG(NavGrid, Error, TEXT("%s: Unable to find NavGrid"), *GetName());
		}
	}
}

void UNavTileComponent::OnComponentCreated()
{
	Super::OnComponentCreated();

	Grid = ANavGrid::GetNavGrid(GetWorld());
	if (Grid && Grid->bDrawTileDebugFigures)
	{
		DrawDebugFigures();
	}
}

bool UNavTileComponent::Traversable(float MaxWalkAngle, const TArray<EGridMovementMode>& AvailableMovementModes) const
{
	FRotator TileRot = GetComponentRotation();
	float MaxAngle = FMath::Max3<float>(TileRot.Pitch, TileRot.Yaw, TileRot.Roll);
	float MinAngle = FMath::Min3<float>(TileRot.Pitch, TileRot.Yaw, TileRot.Roll);
	if (AvailableMovementModes.Contains(EGridMovementMode::Walking) &&
		(MaxAngle < MaxWalkAngle && MinAngle > -MaxWalkAngle))
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool UNavTileComponent::LegalPositionAtEndOfTurn(float MaxWalkAngle, const TArray<EGridMovementMode> &AvailableMovementModes) const
{
	return Traversable(MaxWalkAngle, AvailableMovementModes);
}

void UNavTileComponent::ResetPath()
{
	Distance = std::numeric_limits<float>::infinity();
	Backpointer = NULL;
	Visited = false;
}

TArray<FVector>* UNavTileComponent::GetContactPoints()
{	
	if (!ContactPoints.Num())
	{
		int32 XExtent = Extent->GetScaledBoxExtent().X;
		int32 YExtent = Extent->GetScaledBoxExtent().Y;
		for (int32 X = -XExtent; X <= XExtent; X += XExtent)
		{
			for (int32 Y = -YExtent; Y <= YExtent; Y += YExtent)
			{
				FVector PointLocation = GetComponentRotation().RotateVector(FVector(X, Y, 0));
				FVector WorldLocation = GetComponentLocation() + PointLocation;
				ContactPoints.Add(WorldLocation);
			}
		}
	}
	return &ContactPoints;
}

TArray<UNavTileComponent*>* UNavTileComponent::GetNeighbours()
{
	// Find neighbours if we have not already done so
	if (!Neighbours.Num())
	{
		for (TObjectIterator<UNavTileComponent> Itr; Itr; ++Itr)
		{
			if (Itr->GetWorld() == GetWorld() && *Itr != this)
			{
				bool Added = false; // stop comparing CPs when we know a tile is a neighbour
				for (const FVector &OtherCP : *Itr->GetContactPoints())
				{
					for (const FVector &MyCP : *GetContactPoints())
					{
						if ((OtherCP - MyCP).Size() < 25)
						{
							Neighbours.Add(*Itr);
							Added = true;
							break;
						}
					}
					if (Added) { break; }
				}
			}
		}
	}
	return &Neighbours;
}

bool UNavTileComponent::Obstructed(const FVector &FromPos, const UCapsuleComponent &CollisionCapsule)
{
	return Obstructed(FromPos, PawnLocationOffset->GetComponentLocation(), CollisionCapsule);
}

bool UNavTileComponent::Obstructed(const FVector & From, const FVector & To, const UCapsuleComponent & CollisionCapsule)
{
	FHitResult OutHit;
	FVector Start = From + CollisionCapsule.RelativeLocation;
	FVector End = To + CollisionCapsule.RelativeLocation;
	FQuat Rot = FQuat::Identity;
	FCollisionShape CollisionShape = CollisionCapsule.GetCollisionShape();
	FCollisionQueryParams CQP;
	CQP.AddIgnoredActor(CollisionCapsule.GetOwner());
	FCollisionResponseParams CRP;
	bool HitSomething = CollisionCapsule.GetWorld()->SweepSingleByChannel(OutHit, Start, End, Rot, ECollisionChannel::ECC_Pawn, CollisionShape, CQP, CRP);
/*
	if (HitSomething)
	{
		DrawDebugLine(CollisionCapsule.GetWorld(), Start, End, FColor::Red, true);
	}
	else
	{
		DrawDebugLine(CollisionCapsule.GetWorld(), Start, End, FColor::Green, true);
	}
*/
	return HitSomething;
}

void UNavTileComponent::GetUnobstructedNeighbours(const UCapsuleComponent &CollisionCapsule, TArray<UNavTileComponent *> &OutNeighbours)
{
	OutNeighbours.Empty();
	for (auto N : *GetNeighbours())
	{
		if (!N->Obstructed(PawnLocationOffset->GetComponentLocation(), CollisionCapsule)) 
		{ 
			OutNeighbours.Add(N);
		}
	}
}

void UNavTileComponent::DrawDebugFigures()
{
	FVector Offset = FVector(0, 0, 10); // raise the line a bit so it is not hidden by the floor
	LineBatchComponent->Flush();
	for (UNavTileComponent *N : *GetNeighbours())
	{
		LineBatchComponent->DrawLine(GetComponentLocation() + Offset, N->GetComponentLocation() + Offset, FColor::Blue, 0);
	}
	for (const FVector &CP : *GetContactPoints())
	{
		LineBatchComponent->DrawCircle(CP + Offset, FVector(1, 0, 0), FVector(0, 1, 0), FColor::White, 25, 16, 0);
	}
}

void UNavTileComponent::FlushDebugFigures()
{
	LineBatchComponent->Flush();
}

void UNavTileComponent::Clicked(UPrimitiveComponent* TouchedComponent, FKey Key)
{
	if (Grid)
	{
		Grid->TileClicked(*this);
	}
}

void UNavTileComponent::CursorOver(UPrimitiveComponent* TouchedComponent)
{
	HoverCursor->SetVisibility(true);
	if (Grid)
	{
		Grid->TileCursorOver(*this);
	}
}

void UNavTileComponent::EndCursorOver(UPrimitiveComponent* TouchedComponent)
{
	HoverCursor->SetVisibility(false);
	if (Grid)
	{
		Grid->EndTileCursorOver(*this);
	}
}

void UNavTileComponent::AddSplinePoints(const FVector &FromPos, USplineComponent &OutSpline, bool EndTile)
{
	OutSpline.AddSplinePoint(PawnLocationOffset->GetComponentLocation(), ESplineCoordinateSpace::Local);
}

FVector UNavTileComponent::GetSplineMeshUpVector()
{
	return FVector(0, 0, 1);
}

void UNavTileComponent::DestroyComponent(bool PromoteChildren /*= false*/)
{
	Extent->DestroyComponent();
	PawnLocationOffset->DestroyComponent();
	HoverCursor->DestroyComponent();
	LineBatchComponent->DestroyComponent();

	Super::DestroyComponent(PromoteChildren);
}

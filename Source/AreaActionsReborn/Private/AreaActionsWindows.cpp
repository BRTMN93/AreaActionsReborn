#include "AreaActionsWindows.h"
#include "AreaActionsLog.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Components/EditableText.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Kismet/GameplayStatics.h"
#include "Resources/FGItemDescriptor.h"

#define LOCTEXT_NAMESPACE "AreaActions"

namespace
{
	UEditableText* InnerText(const UUserWidget* Box)
	{
		const FObjectPropertyBase* Property = Box ? FindFProperty<FObjectPropertyBase>(Box->GetClass(), TEXT("mInputBox")) : nullptr;
		return Property ? Cast<UEditableText>(Property->GetObjectPropertyValue_InContainer(Box)) : nullptr;
	}

	FText Number(double Value, bool bFloat)
	{
		FNumberFormattingOptions Options;
		Options.UseGrouping = false;
		Options.MaximumFractionalDigits = bFloat ? 2 : 0;
		return FText::AsNumber(bFloat ? Value : FMath::RoundToDouble(Value), &Options);
	}
}

// ---- SetRecipe ----

FText UAreaActionsRecipeWindow::GetWindowTitle() const
{
	return LOCTEXT("RecipeWindowTitle", "Select recipe");
}

UWidget* UAreaActionsRecipeWindow::BuildContent()
{
	UWidgetTree* Tree = WidgetTree;
	UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RecipeRoot"));
	if (UUserWidget* Search = AreaActionsUI::MakeGame(Tree, AreaActionsUI::InputBoxPath, TEXT("SearchBox")))
	{
		AreaActionsUI::SetText(Search, TEXT("mPreviewText"), LOCTEXT("RecipeSearch", "Search"));
		AreaActionsUI::Bind(Search, TEXT("OnTextChanged"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsRecipeWindow, SearchChanged));
		Root->AddChildToVerticalBox(Search)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	}
	UVerticalBox* Box = nullptr;
	UUserWidget* Scroll = AreaActionsUI::MakeScroll(Tree, Box, TEXT("RecipeList"));
	if (!Scroll)
	{
		return Root;
	}
	List = Box;
	Root->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	// The recipes a factory machine makes, as the original listed them (not the Build Gun's and the workshops'), sorted by name
	TArray<TSubclassOf<UFGRecipe>> All;
	if (AFGRecipeManager* Manager = AFGRecipeManager::Get(GetOwningPlayer()))
	{
		Manager->GetAllAvailableRecipes(All);
	}
	UFGRecipe::SortByName(All);
	const double Started = FPlatformTime::Seconds();
	for (const TSubclassOf<UFGRecipe>& Recipe : All)
	{
		const bool bMachine = Recipe && UFGRecipe::GetProducedIn(Recipe).ContainsByPredicate([](const TSubclassOf<UObject>& Producer)
		{
			return Producer && Producer->IsChildOf(AFGBuildableManufacturer::StaticClass());
		});
		if (!bMachine)
		{
			continue;
		}
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
		UTexture2D* Icon = Products.IsEmpty() ? nullptr : UFGItemDescriptor::GetSmallIcon(Products[0].ItemClass);
		if (UUserWidget* Button = AreaActionsUI::MakeListButton(Tree, *(TEXT("Recipe_") + Recipe->GetName()), UFGRecipe::GetRecipeName(Recipe), FText::GetEmpty(), Icon,
			Box, this, GET_FUNCTION_NAME_CHECKED(UAreaActionsRecipeWindow, RecipeClicked)))
		{
			Box->AddChildToVerticalBox(Button);
			Recipes.Add(Button, Recipe);
		}
	}
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Recipe window: %d recipes of %d, built in %.1f ms"), Recipes.Num(), All.Num(), (FPlatformTime::Seconds() - Started) * 1000.0);
	return Root;
}

void UAreaActionsRecipeWindow::SearchChanged(FText Text)
{
	const FString Search = Text.ToString().TrimStartAndEnd();
	for (const TPair<TObjectPtr<UUserWidget>, TSubclassOf<UFGRecipe>>& Entry : Recipes)
	{
		const bool bShown = Search.IsEmpty() || UFGRecipe::GetRecipeName(Entry.Value).ToString().Contains(Search, ESearchCase::IgnoreCase);
		Entry.Key->SetVisibility(bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UAreaActionsRecipeWindow::RecipeClicked(int32 Index, UUserWidget* Button)
{
	const TSubclassOf<UFGRecipe>* Recipe = Recipes.Find(Button);
	if (!Recipe || bHasResult)
	{
		return;
	}
	bHasResult = true;
	OnRecipeSelected.Broadcast(*Recipe);
	CloseWindow();
}

// ---- Copy and Fill: shared ----

UWidget* UAreaActionsPlaceWindow::MakeField(FName Name, const FText& Label, const FLinearColor& LabelColor, const FField& Field)
{
	UWidgetTree* Tree = WidgetTree;
	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
	USizeBox* LabelSize = Tree->ConstructWidget<USizeBox>();
	LabelSize->SetWidthOverride(28.0f);
	UTextBlock* Text = AreaActionsUI::MakeText(Tree, Label, 16.0f, TEXT("Bold"), LabelColor);
	Text->SetAutoWrapText(false);
	LabelSize->AddChild(Text);
	UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelSize);
	LabelSlot->SetVerticalAlignment(VAlign_Center);
	UUserWidget* Box = AreaActionsUI::MakeGame(Tree, AreaActionsUI::InputBoxPath, Name);
	if (Box)
	{
		AreaActionsUI::SetBool(Box, TEXT("isSearchbar"), false);
		AreaActionsUI::SetBool(Box, TEXT("mShowGamepadHint"), false);
		AreaActionsUI::Bind(Box, TEXT("OnTextComitted"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPlaceWindow, FieldCommitted));
		UHorizontalBoxSlot* BoxSlot = Row->AddChildToHorizontalBox(Box);
		BoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		FField Stored = Field;
		Stored.Box = Box;
		Fields.Add(Name, Stored);
	}
	return Row;
}

UWidget* UAreaActionsPlaceWindow::MakeCostAndDistance()
{
	UWidgetTree* Tree = WidgetTree;
	UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
	Box->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, LOCTEXT("CostHeading", "Build cost")));
	UHorizontalBox* CostRow = Tree->ConstructWidget<UHorizontalBox>();
	CostToggle = AreaActionsUI::MakeGame(Tree, AreaActionsUI::TogglePath, TEXT("UseBuildCost"));
	if (CostToggle)
	{
		AreaActionsUI::Bind(CostToggle, TEXT("OnToggled"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPlaceWindow, CostToggled));
		USizeBox* ToggleSize = Tree->ConstructWidget<USizeBox>();
		ToggleSize->SetWidthOverride(42.0f);
		ToggleSize->SetHeightOverride(20.0f);
		ToggleSize->AddChild(CostToggle);
		CostRow->AddChildToHorizontalBox(ToggleSize)->SetVerticalAlignment(VAlign_Center);
	}
	UHorizontalBoxSlot* CostText = CostRow->AddChildToHorizontalBox(AreaActionsUI::MakeText(Tree, LOCTEXT("UseCost", "Use build cost (from your inventory, containers in range and the Dimensional Depot)"), 14.0f));
	CostText->SetPadding(FMargin(10.0f, 0.0f, 0.0f, 0.0f));
	CostText->SetVerticalAlignment(VAlign_Center);
	CostText->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Box->AddChildToVerticalBox(CostRow)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	// The container distance: the game's options slider (a bar filled to the value, the value's field beside it)
	UHorizontalBox* DistanceRow = Tree->ConstructWidget<UHorizontalBox>();
	UHorizontalBoxSlot* DistanceLabel = DistanceRow->AddChildToHorizontalBox(AreaActionsUI::MakeText(Tree, LOCTEXT("MaxDistance", "Max container distance"), 14.0f));
	DistanceLabel->SetVerticalAlignment(VAlign_Center);
	DistanceLabel->SetPadding(FMargin(0.0f, 0.0f, 12.0f, 0.0f));
	UOverlay* Slide = Tree->ConstructWidget<UOverlay>();
	DistanceBar = Tree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("MaxDistBar"));
	FProgressBarStyle BarStyle;
	BarStyle.BackgroundImage.TintColor = FSlateColor(AreaActionsUI::Dark);
	BarStyle.FillImage.TintColor = FSlateColor(FLinearColor::White);
	DistanceBar->SetWidgetStyle(BarStyle);
	DistanceBar->SetFillColorAndOpacity(AreaActionsUI::Orange);
	UOverlaySlot* BarSlot = Slide->AddChildToOverlay(DistanceBar);
	BarSlot->SetHorizontalAlignment(HAlign_Fill);
	BarSlot->SetVerticalAlignment(VAlign_Fill);
	DistanceSlider = Tree->ConstructWidget<USlider>(USlider::StaticClass(), TEXT("MaxDistSlider"));
	FSliderStyle SliderStyle;
	SliderStyle.NormalBarImage.TintColor = FSlateColor(FLinearColor::Transparent);
	SliderStyle.HoveredBarImage.TintColor = FSlateColor(FLinearColor::Transparent);
	SliderStyle.DisabledBarImage.TintColor = FSlateColor(FLinearColor::Transparent);
	SliderStyle.NormalThumbImage.DrawAs = ESlateBrushDrawType::NoDrawType;
	SliderStyle.HoveredThumbImage.DrawAs = ESlateBrushDrawType::NoDrawType;
	SliderStyle.DisabledThumbImage.DrawAs = ESlateBrushDrawType::NoDrawType;
	DistanceSlider->SetWidgetStyle(SliderStyle);
	DistanceSlider->OnValueChanged.AddDynamic(this, &UAreaActionsPlaceWindow::DistanceSlid);
	UOverlaySlot* SliderSlot = Slide->AddChildToOverlay(DistanceSlider);
	SliderSlot->SetHorizontalAlignment(HAlign_Fill);
	SliderSlot->SetVerticalAlignment(VAlign_Fill);
	USizeBox* SlideSize = Tree->ConstructWidget<USizeBox>();
	SlideSize->SetHeightOverride(32.0f);
	SlideSize->AddChild(Slide);
	UHorizontalBoxSlot* SlideSlot = DistanceRow->AddChildToHorizontalBox(SlideSize);
	SlideSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	SlideSlot->SetVerticalAlignment(VAlign_Center);
	USizeBox* FieldSize = Tree->ConstructWidget<USizeBox>();
	FieldSize->SetWidthOverride(150.0f);
	FField Distance;
	Distance.Min = 0.0;
	FieldSize->AddChild(MakeField(TEXT("MaxDistNumberBox"), FText::GetEmpty(), FLinearColor::White, Distance));
	DistanceRow->AddChildToHorizontalBox(FieldSize)->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	UHorizontalBoxSlot* Unit = DistanceRow->AddChildToHorizontalBox(AreaActionsUI::MakeText(Tree, LOCTEXT("Metres", "m"), 14.0f));
	Unit->SetVerticalAlignment(VAlign_Center);
	Unit->SetPadding(FMargin(6.0f, 0.0f, 0.0f, 0.0f));
	Box->AddChildToVerticalBox(DistanceRow);

	// The values the player used last (the settings actor, D21)
	if (const AActor* Settings = GetSettings())
	{
		const FDoubleProperty* Now = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("ContainerDistance"));
		const FDoubleProperty* Max = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("MaxContainerDistance"));
		const double Value = Now ? Now->GetPropertyValue_InContainer(Settings) : 0.0;
		const double Top = Max ? FMath::Max(1.0, Max->GetPropertyValue_InContainer(Settings)) : 1.0;
		DistanceSlider->SetValue(static_cast<float>(Value / Top));
		DistanceBar->SetPercent(static_cast<float>(Value / Top));
		SetNumber(TEXT("MaxDistNumberBox"), Value / 100.0);
	}
	return Box;
}

UHorizontalBox* UAreaActionsPlaceWindow::MakeButtonRow()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ButtonsBox"));
	return Row;
}

AActor* UAreaActionsPlaceWindow::GetSettings() const
{
	UClass* Class = LoadClass<AActor>(nullptr, TEXT("/AreaActionsReborn/Actions/ActionsSettings.ActionsSettings_C"));
	return Class ? UGameplayStatics::GetActorOfClass(GetOwningPlayer(), Class) : nullptr;
}

double UAreaActionsPlaceWindow::GetNumber(FName Name) const
{
	const FField* Field = Fields.Find(Name);
	const UEditableText* Text = Field ? InnerText(Field->Box) : nullptr;
	if (!Text)
	{
		return Field ? Field->Default : 0.0;
	}
	FString Typed = Text->GetText().ToString().TrimStartAndEnd().Replace(TEXT(","), TEXT("."));
	double Value = Field->Default;
	if (Typed.IsEmpty() || !LexTryParseString(Value, *Typed) || !FMath::IsFinite(Value))
	{
		Value = Field->Default;
	}
	if (!Field->bFloat)
	{
		Value = FMath::RoundToDouble(Value);
	}
	if (!Field->bAllowZero && Value == 0.0)
	{
		Value = Field->Default;
	}
	return FMath::Clamp(Value, Field->Min, Field->Max);
}

void UAreaActionsPlaceWindow::SetNumber(FName Name, double Value)
{
	FField* Field = Fields.Find(Name);
	if (!Field)
	{
		return;
	}
	Field->Shown = Value;
	if (UEditableText* Text = InnerText(Field->Box))
	{
		Text->SetText(Number(Value, Field->bFloat));
		// The game's input box draws its own text block (InputText), which follows the edit box only as the player types
		Text->OnTextChanged.Broadcast(Text->GetText());
	}
}

void UAreaActionsPlaceWindow::NativeConstruct()
{
	Super::NativeConstruct();
	// The game's input boxes empty their text when they are constructed, after BuildContent filled them: their values again
	for (TPair<FName, FField>& Entry : Fields)
	{
		SetNumber(Entry.Key, Entry.Value.Shown);
	}
}

void UAreaActionsPlaceWindow::Normalize(FName Name)
{
	SetNumber(Name, GetNumber(Name));
}

void UAreaActionsPlaceWindow::FieldCommitted(FText Text, uint8 CommitMethod)
{
	// The dispatcher does not say which box: every box gets its value cleaned, as the old number boxes did on commit
	TArray<FName> Names;
	Fields.GetKeys(Names);
	for (const FName& Name : Names)
	{
		Normalize(Name);
	}
	// The distance field moves the slider and is kept in the settings actor
	if (AActor* Settings = GetSettings())
	{
		const double Metres = GetNumber(TEXT("MaxDistNumberBox"));
		if (FDoubleProperty* Now = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("ContainerDistance")))
		{
			Now->SetPropertyValue_InContainer(Settings, Metres * 100.0);
		}
		const FDoubleProperty* Max = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("MaxContainerDistance"));
		const double Top = Max ? FMath::Max(1.0, Max->GetPropertyValue_InContainer(Settings)) : 1.0;
		if (DistanceSlider)
		{
			DistanceSlider->SetValue(static_cast<float>(Metres * 100.0 / Top));
			DistanceBar->SetPercent(static_cast<float>(Metres * 100.0 / Top));
		}
	}
	FieldChanged(NAME_None);
}

void UAreaActionsPlaceWindow::DistanceSlid(float Value)
{
	AActor* Settings = GetSettings();
	const FDoubleProperty* Max = Settings ? FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("MaxContainerDistance")) : nullptr;
	FDoubleProperty* Now = Settings ? FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("ContainerDistance")) : nullptr;
	const double Top = Max ? FMath::Max(1.0, Max->GetPropertyValue_InContainer(Settings)) : 1.0;
	const double Metres = FMath::RoundToDouble(Value * Top) / 100.0;
	if (Now)
	{
		Now->SetPropertyValue_InContainer(Settings, Metres * 100.0);
	}
	if (DistanceBar)
	{
		DistanceBar->SetPercent(Value);
	}
	SetNumber(TEXT("MaxDistNumberBox"), Metres);
}

bool UAreaActionsPlaceWindow::GetUseCost() const
{
	return CostToggle && AreaActionsUI::GetBool(CostToggle, TEXT("mToggleIsOn"));
}

void UAreaActionsPlaceWindow::SetUseCost(bool bUse)
{
	if (CostToggle)
	{
		// SetIsOn(isOn, CallDelegate = false)
		AreaActionsUI::CallBool(CostToggle, TEXT("SetIsOn"), bUse);
		AreaActionsUI::SetBool(CostToggle, TEXT("mToggleIsOn"), bUse);
	}
}

void UAreaActionsPlaceWindow::CostToggled(bool bOn)
{
	UE_LOG(LogAreaActionsCpp, Display, TEXT("%s: use build cost %d"), *GetClass()->GetName(), bOn ? 1 : 0);
}

void UAreaActionsPlaceWindow::HideClicked()
{
	HideForPick();
}

void UAreaActionsPlaceWindow::HideForPick()
{
	SetVisibility(ESlateVisibility::Hidden);
	if (APlayerController* Player = GetOwningPlayer())
	{
		UWidgetBlueprintLibrary::SetInputMode_GameOnly(Player, false);
		Player->SetShowMouseCursor(false);
	}
}

// ---- Copy ----

FText UAreaActionsCopyWindow::GetWindowTitle() const
{
	return LOCTEXT("CopyWindowTitle", "Copy settings");
}

UWidget* UAreaActionsCopyWindow::BuildContent()
{
	UWidgetTree* Tree = WidgetTree;
	UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CopyRoot"));
	UHorizontalBox* Columns = Tree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* Offset = Tree->ConstructWidget<UVerticalBox>();
	Offset->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, LOCTEXT("CopyOffset", "Offset (m, along the area's axes)")));
	FField Metres;
	Offset->AddChildToVerticalBox(MakeField(TEXT("XNumberBox"), INVTEXT("X"), FLinearColor(0.9f, 0.25f, 0.2f), Metres))->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	Offset->AddChildToVerticalBox(MakeField(TEXT("YNumberBox"), INVTEXT("Y"), FLinearColor(0.35f, 0.85f, 0.3f), Metres))->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	Offset->AddChildToVerticalBox(MakeField(TEXT("ZNumberBox"), INVTEXT("Z"), FLinearColor(0.3f, 0.55f, 1.0f), Metres));
	UHorizontalBoxSlot* OffsetSlot = Columns->AddChildToHorizontalBox(Offset);
	OffsetSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	OffsetSlot->SetPadding(FMargin(0.0f, 0.0f, 20.0f, 0.0f));
	UVerticalBox* Rotation = Tree->ConstructWidget<UVerticalBox>();
	Rotation->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, LOCTEXT("CopyRotation", "Rotation (degrees)")));
	FField Degrees;
	Degrees.Min = -180.0;
	Degrees.Max = 180.0;
	Rotation->AddChildToVerticalBox(MakeField(TEXT("RotationZ"), INVTEXT("Z"), FLinearColor(0.3f, 0.55f, 1.0f), Degrees))->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	Rotation->AddChildToVerticalBox(AreaActionsUI::MakeButton(Tree, TEXT("SetRotationCenter"), LOCTEXT("SetRotationCenter", "Set rotation center"), this,
		GET_FUNCTION_NAME_CHECKED(UAreaActionsCopyWindow, RotationCenterPressed)));
	Columns->AddChildToHorizontalBox(Rotation)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	for (const TPair<FName, FField>& Entry : Fields)
	{
		SetNumber(Entry.Key, Entry.Value.Default);
	}
	Root->AddChildToVerticalBox(Columns)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
	Root->AddChildToVerticalBox(MakeCostAndDistance());
	Root->AddChildToVerticalBox(Tree->ConstructWidget<USpacer>())->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UHorizontalBox* Buttons = MakeButtonRow();
	auto Add = [Buttons](UWidget* Button, bool bFill)
	{
		UHorizontalBoxSlot* ButtonSlot = Buttons->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(4.0f, 0.0f));
		ButtonSlot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
	};
	Add(AreaActionsUI::MakeButton(Tree, TEXT("PasteTo"), LOCTEXT("PasteTo", "Paste to location"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsCopyWindow, PasteToPressed)), true);
	Add(AreaActionsUI::MakeButton(Tree, TEXT("HideButton"), LOCTEXT("Hide", "Hide"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPlaceWindow, HideClicked)), true);
	Add(AreaActionsUI::MakeButton(Tree, TEXT("PreviewButton"), LOCTEXT("Preview", "Preview"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsCopyWindow, PreviewPressed)), true);
	Add(AreaActionsUI::MakeButton(Tree, TEXT("OkButton"), LOCTEXT("CopyOk", "Copy"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsCopyWindow, OkPressed), true), true);
	Root->AddChildToVerticalBox(Buttons);
	return Root;
}

void UAreaActionsCopyWindow::GetData(FVector& Delta, FRotator& DeltaRotation) const
{
	Delta = FVector(GetNumber(TEXT("XNumberBox")), GetNumber(TEXT("YNumberBox")), GetNumber(TEXT("ZNumberBox"))) * 100.0;
	DeltaRotation = FRotator(0.0, GetNumber(TEXT("RotationZ")), 0.0);
}

void UAreaActionsCopyWindow::SetData(FVector Delta, FRotator DeltaRotation)
{
	SetNumber(TEXT("XNumberBox"), Delta.X);
	SetNumber(TEXT("YNumberBox"), Delta.Y);
	SetNumber(TEXT("ZNumberBox"), Delta.Z);
	SetNumber(TEXT("RotationZ"), DeltaRotation.Yaw);
}

void UAreaActionsCopyWindow::OkPressed()
{
	if (bHasResult)
	{
		return;
	}
	bHasResult = true;
	FVector Delta;
	FRotator Rotation;
	GetData(Delta, Rotation);
	OkClicked.Broadcast(Delta, Rotation, GetUseCost());
	CloseWindow();
}

void UAreaActionsCopyWindow::PreviewPressed()
{
	FVector Delta;
	FRotator Rotation;
	GetData(Delta, Rotation);
	Preview.Broadcast(Delta, Rotation);
}

void UAreaActionsCopyWindow::PasteToPressed()
{
	IsSelectingPasteTo = true;
	HideForPick();
}

void UAreaActionsCopyWindow::RotationCenterPressed()
{
	IsSelectingRotationPoint = true;
	HideForPick();
}

// ---- Fill ----

FText UAreaActionsFillWindow::GetWindowTitle() const
{
	return LOCTEXT("FillWindowTitle", "Fill settings");
}

UWidget* UAreaActionsFillWindow::BuildContent()
{
	UWidgetTree* Tree = WidgetTree;
	UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("FillRoot"));
	UHorizontalBox* Columns = Tree->ConstructWidget<UHorizontalBox>();
	const FLinearColor Red(0.9f, 0.25f, 0.2f), Green(0.35f, 0.85f, 0.3f), Blue(0.3f, 0.55f, 1.0f);
	auto Column = [Tree, Columns](const FText& Heading, bool bLast)
	{
		UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
		Box->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, Heading));
		UHorizontalBoxSlot* ColumnSlot = Columns->AddChildToHorizontalBox(Box);
		ColumnSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ColumnSlot->SetPadding(FMargin(0.0f, 0.0f, bLast ? 0.0f : 16.0f, 0.0f));
		return Box;
	};
	auto Add = [this](UVerticalBox* Box, FName Name, const TCHAR* Axis, const FLinearColor& Color, const FField& Field)
	{
		Box->AddChildToVerticalBox(MakeField(Name, FText::FromString(Axis), Color, Field))->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	};
	FField Count;
	Count.bFloat = false;
	Count.Default = 1.0;
	Count.bAllowZero = false;
	UVerticalBox* Amount = Column(LOCTEXT("FillAmount", "Copies (minus: back)"), false);
	Add(Amount, TEXT("XAmount"), TEXT("X"), Red, Count);
	Add(Amount, TEXT("YAmount"), TEXT("Y"), Green, Count);
	Add(Amount, TEXT("ZAmount"), TEXT("Z"), Blue, Count);
	FField Metres;
	UVerticalBox* Border = Column(LOCTEXT("FillBorder", "Gap between copies (m)"), false);
	Add(Border, TEXT("XBorder"), TEXT("X"), Red, Metres);
	Add(Border, TEXT("YBorder"), TEXT("Y"), Green, Metres);
	Add(Border, TEXT("ZBorder"), TEXT("Z"), Blue, Metres);
	UVerticalBox* Ramp = Column(LOCTEXT("FillRamp", "Ramp (m per copy)"), true);
	Add(Ramp, TEXT("XRamp"), TEXT("X"), Red, Metres);
	Add(Ramp, TEXT("YRamp"), TEXT("Y"), Green, Metres);
	for (const TPair<FName, FField>& Entry : Fields)
	{
		SetNumber(Entry.Key, Entry.Value.Default);
	}
	Root->AddChildToVerticalBox(Columns)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
	Root->AddChildToVerticalBox(MakeCostAndDistance());
	Root->AddChildToVerticalBox(Tree->ConstructWidget<USpacer>())->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UHorizontalBox* Buttons = MakeButtonRow();
	auto Button = [Buttons](UWidget* Made)
	{
		UHorizontalBoxSlot* ButtonSlot = Buttons->AddChildToHorizontalBox(Made);
		ButtonSlot->SetPadding(FMargin(4.0f, 0.0f));
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	};
	Button(AreaActionsUI::MakeButton(Tree, TEXT("FillTo"), LOCTEXT("FillTo", "Fill to location"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsFillWindow, FillToPressed)));
	Button(AreaActionsUI::MakeButton(Tree, TEXT("HideButton"), LOCTEXT("Hide", "Hide"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPlaceWindow, HideClicked)));
	Button(AreaActionsUI::MakeButton(Tree, TEXT("PreviewButton"), LOCTEXT("Preview", "Preview"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsFillWindow, PreviewPressed)));
	Button(AreaActionsUI::MakeButton(Tree, TEXT("OkButton"), LOCTEXT("FillOk", "Fill"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsFillWindow, OkPressed), true));
	Root->AddChildToVerticalBox(Buttons);
	return Root;
}

void UAreaActionsFillWindow::GetData(FVector& Border, FIntVector& Amount, FVector& Ramp) const
{
	Border = FVector(GetNumber(TEXT("XBorder")), GetNumber(TEXT("YBorder")), GetNumber(TEXT("ZBorder"))) * 100.0;
	Amount = FIntVector(static_cast<int32>(GetNumber(TEXT("XAmount"))), static_cast<int32>(GetNumber(TEXT("YAmount"))), static_cast<int32>(GetNumber(TEXT("ZAmount"))));
	Ramp = FVector(GetNumber(TEXT("XRamp")), GetNumber(TEXT("YRamp")), 0.0) * 100.0;
}

void UAreaActionsFillWindow::SetData(FIntVector Amount)
{
	SetNumber(TEXT("XAmount"), Amount.X);
	SetNumber(TEXT("YAmount"), Amount.Y);
	SetNumber(TEXT("ZAmount"), Amount.Z);
}

void UAreaActionsFillWindow::FieldChanged(FName Field)
{
	UpdateIndicatorRamp.Broadcast();
}

void UAreaActionsFillWindow::OkPressed()
{
	if (bHasResult)
	{
		return;
	}
	bHasResult = true;
	FVector Border, Ramp;
	FIntVector Amount;
	GetData(Border, Amount, Ramp);
	OkClicked.Broadcast(Border, Amount, Ramp, GetUseCost());
	CloseWindow();
}

void UAreaActionsFillWindow::PreviewPressed()
{
	FVector Border, Ramp;
	FIntVector Amount;
	GetData(Border, Amount, Ramp);
	Preview.Broadcast(Border, Amount, Ramp);
}

void UAreaActionsFillWindow::FillToPressed()
{
	IsSelectingFillTo = true;
	HideForPick();
}

#undef LOCTEXT_NAMESPACE

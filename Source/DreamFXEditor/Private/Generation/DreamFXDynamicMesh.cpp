#include "Generation/DreamFXDynamicMesh.h"

#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Dom/JsonObject.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Generation/DreamFXProvenance.h"
#include "Generation/DreamFXValueLowering.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lint/DreamFXLint.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SourceFiles/DreamFXPaths.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/**
		 * Mixed into the system's provenance hash. The generated module text is hashed too, so a change
		 * that reaches a module rebuilds on its own; this covers changes that only reach the system side
		 * (emitter settings, the data interface configuration) and would otherwise leave an up-to-date
		 * stamp on an asset the current lowering would build differently. Bump it with any such change.
		 */
		const TCHAR* const LoweringVersion = TEXT("dynamic-mesh-1");

		const TCHAR* const ModuleFolderSuffix = TEXT("_DynamicMesh");
		const TCHAR* const TopologyModuleName = TEXT("DM_GridTopology");
		const TCHAR* const VisibilityModuleName = TEXT("DM_Visibility");
		const TCHAR* const VertexAttribute = TEXT("Particles.DynamicMesh.Vertex");

		const TCHAR* const EmitterStateModule = TEXT("/Niagara/Modules/Emitter/EmitterState");
		const TCHAR* const SpawnBurstModule = TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous");
		const TCHAR* const InitializeParticleModule = TEXT("/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle");
		const TCHAR* const ParticleStateModule = TEXT("/Niagara/Modules/Update/Lifetime/ParticleState");

		constexpr int32 MaxTexCoords = 8;
		/** Index arithmetic in the generated HLSL is float, exact for integers up to 2^24. */
		constexpr int64 MaxPatchVertices = int64(1) << 24;
		/** "As long as the system runs": a lifetime no effect reaches, and an age float still resolves. */
		constexpr double EndlessLifetime = 1.0e8;

		/** Names the generated prologue declares; an input with one of them would not compile. */
		const TCHAR* const ReservedNames[] = {
			TEXT("U"), TEXT("V"), TEXT("I"), TEXT("J"), TEXT("NU"), TEXT("NV"), TEXT("K"), TEXT("NK"),
			TEXT("Rand"), TEXT("T"), TEXT("Index"), TEXT("Theta"), TEXT("Phi"), TEXT("Dir"),
			TEXT("Position"), TEXT("Color"), TEXT("Normal"), TEXT("Tangent"),
			TEXT("UV0"), TEXT("UV1"), TEXT("UV2"), TEXT("UV3"), TEXT("UV4"), TEXT("UV5"), TEXT("UV6"), TEXT("UV7"),
		};

		enum class ENormalMode : uint8 { None, Auto, Explicit };
		enum class ETopology : uint8 { Grid, Strip, Ring, Tube, Sphere };

		struct FTopologyInfo
		{
			ETopology Kind;
			const TCHAR* Name;
			int32 Counts;
		};

		const FTopologyInfo Topologies[] = {
			{ ETopology::Grid, TEXT("Grid"), 2 },
			{ ETopology::Strip, TEXT("Strip"), 1 },
			{ ETopology::Ring, TEXT("Ring"), 2 },
			{ ETopology::Tube, TEXT("Tube"), 2 },
			{ ETopology::Sphere, TEXT("Sphere"), 2 },
		};

		struct FPatchPlan
		{
			const FDynamicMeshPatch* Source = nullptr;
			ETopology Kind = ETopology::Grid;
			FString KindName;
			int32 Columns = 2;
			int32 Rows = 2;
			int32 Copies = 1;
			bool bFlip = false;
			/** This patch's own material object path; empty when it uses the mesh's. */
			FString Material;
			bool bHasMaterial = false;
			int32 Section = 0;
			int64 VertexBase = 0;
			int64 VertexCount = 0;
			int64 TriangleBase = 0;
			int64 TriangleCount = 0;
			FString EmitterName;
			FString ModuleName;
			FString ModulePath;
		};

		struct FMeshPlan
		{
			const FDynamicMesh* Source = nullptr;
			FString Material;
			int32 TexCoords = 1;
			bool bColors = true;
			ENormalMode Normals = ENormalMode::None;
			FValuePtr Bounds;
			double BoundsMin[3] = { -1000.0, -1000.0, -1000.0 };
			double BoundsMax[3] = { 1000.0, 1000.0, 1000.0 };
			TOptional<double> Duration;
			FValuePtr Visible;
			FSourceLocation VisibleLocation;
			/** Section material object paths ("" = engine default), in order of first use. */
			TArray<FString> SectionMaterials;
			TArray<int64> SectionTriangles;
			TArray<FPatchPlan> Patches;
			int64 TotalVertices = 0;
			int64 TotalTriangles = 0;
			int32 TexCoordStride = 1;
		};

		// ----------------------------------------------------------------------------- AST helpers

		FValuePtr MakeInt(int64 Value, const FSourceLocation& Location)
		{
			return FValue::MakeNumber(static_cast<double>(Value), /*bInteger=*/true, Location);
		}

		FValuePtr MakeFloat(double Value, const FSourceLocation& Location)
		{
			return FValue::MakeNumber(Value, /*bInteger=*/false, Location);
		}

		FValuePtr MakeVector(std::initializer_list<double> Components, const FSourceLocation& Location)
		{
			FValuePtr Node = FValue::Make(EValueKind::Vector, Location);
			for (const double Component : Components)
			{
				Node->Elements.Add(MakeFloat(Component, Location));
			}
			return Node;
		}

		FValuePtr MakeStringArray(std::initializer_list<FString> Strings, const FSourceLocation& Location)
		{
			FValuePtr Node = FValue::Make(EValueKind::Array, Location);
			for (const FString& String : Strings)
			{
				Node->Elements.Add(FValue::MakeString(String, Location));
			}
			return Node;
		}

		FNamedArgument MakeArgument(const TCHAR* Name, const FValuePtr& Value)
		{
			FNamedArgument Argument;
			Argument.Name = Name;
			Argument.Value = Value;
			Argument.Location = Value.IsValid() ? Value->Location : FSourceLocation();
			return Argument;
		}

		FStatement MakeCall(const FString& Module, TArray<FNamedArgument> Arguments, const FSourceLocation& Location,
			const FString& SourceFile)
		{
			FStatement Statement;
			Statement.Kind = EStatementKind::ModuleCall;
			Statement.Name = Module;
			Statement.Arguments = MoveTemp(Arguments);
			Statement.Location = Location;
			Statement.SourceFile = SourceFile;
			return Statement;
		}

		FPropertyEntry MakeProperty(const TCHAR* Name, const FValuePtr& Value, const FSourceLocation& Location,
			const FString& SourceFile)
		{
			FPropertyEntry Property;
			Property.Name = Name;
			Property.Value = Value;
			Property.Location = Location;
			Property.SourceFile = SourceFile;
			return Property;
		}

		FStack MakeStack(EStackKind Kind, const FSourceLocation& Location, const FString& SourceFile)
		{
			FStack Stack;
			Stack.Kind = Kind;
			Stack.Location = Location;
			Stack.SourceFile = SourceFile;
			return Stack;
		}

		bool IsIntegerNumber(const FValuePtr& Value)
		{
			return Value.IsValid() && Value->Kind == EValueKind::Number && Value->bIsIntegerLiteral;
		}

		FString FormatFloat(double Value)
		{
			// HLSL needs the decimal point: `3` is an int literal and changes what `/` means.
			FString Text = FString::SanitizeFloat(Value);
			if (!Text.Contains(TEXT(".")) && !Text.Contains(TEXT("e")) && !Text.Contains(TEXT("E")))
			{
				Text += TEXT(".0");
			}
			return Text;
		}

		FString FormatCount(int64 Value)
		{
			return FString::Printf(TEXT("%lld.0"), Value);
		}

		/** Where generated modules live: a folder named after the system, next to it. */
		FString ModuleRelativeName(const FDocument& System, const FString& AssetName)
		{
			FString Name = System.Name;
			Name.RemoveFromStart(TEXT("/"));
			return FString::Printf(TEXT("%s%s/%s"), *Name, ModuleFolderSuffix, *AssetName);
		}

		bool ModuleFullPath(const FDocument& System, const FString& AssetName, FString& OutPath,
			FDiagnosticSink& Diagnostics, const FSourceLocation& Location)
		{
			FString MountPoint;
			FString Error;
			if (!FDreamFXPaths::ResolveRootMountPoint(System.Root, MountPoint, Error))
			{
				Diagnostics.Error(TEXT("DFX5101"), Location, Error);
				return false;
			}
			OutPath = MountPoint / ModuleRelativeName(System, AssetName);
			return true;
		}

		FString DumpPath(const FDocument& System, const FString& AssetName)
		{
			FString Root = System.Root.IsEmpty() ? FString(TEXT("Game")) : System.Root.Replace(TEXT("Plugin."), TEXT(""));
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamFX/DynamicMesh") / Root
				/ ModuleRelativeName(System, AssetName) + TEXT(".dfm"));
		}

		/**
		 * Mesh and patch names become emitter names, asset names and HLSL comments, so only the spelling
		 * all three accept is allowed -- a back-quoted `My Mesh` would make an asset path with a space in it.
		 */
		bool IsPlainName(const FString& Name)
		{
			if (Name.IsEmpty() || FChar::IsDigit(Name[0]))
			{
				return false;
			}
			for (const TCHAR Character : Name)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		FString QuoteHlslString(const FString& Text)
		{
			return Text.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		}

		// ------------------------------------------------------------------------------- settings

		bool ResolveMaterial(const FPropertyEntry& Entry, const FString& Root, const FDynamicMeshLowerOptions& Options,
			FString& OutObjectPath, FDiagnosticSink& Diagnostics)
		{
			if (!Entry.Value.IsValid() || Entry.Value->Kind != EValueKind::String)
			{
				Diagnostics.Error(TEXT("DFX3060"), Entry.Location,
					TEXT("Material takes a quoted material asset path, e.g. Material = \"/Game/FX/M_Shell.M_Shell\";"));
				return false;
			}
			FString PackagePath;
			FString Error;
			if (!FDreamFXPaths::ResolveAssetPath(Entry.Value->Text, Root, PackagePath, Error))
			{
				Diagnostics.Error(TEXT("DFX3061"), Entry.Location, FString::Printf(TEXT("Material '%s': %s"), *Entry.Value->Text, *Error));
				return false;
			}
			OutObjectPath = FDreamFXPaths::ToObjectPath(PackagePath);
			if (Options.bResolveAssets
				&& LoadObject<UMaterialInterface>(nullptr, *OutObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet) == nullptr)
			{
				Diagnostics.Error(TEXT("DFX3061"), Entry.Location, FString::Printf(
					TEXT("Material '%s' resolves to '%s', which is not a material asset. A dynamic mesh with a missing material draws with the engine's default one, which is never what was meant."),
					*Entry.Value->Text, *OutObjectPath));
				return false;
			}
			return true;
		}

		bool ReadMeshSettings(const FDocument& System, const FDynamicMesh& Mesh, const FDynamicMeshLowerOptions& Options,
			FMeshPlan& Plan, FDiagnosticSink& Diagnostics)
		{
			bool bOk = true;
			TSet<FString> Seen;
			for (const FPropertyEntry& Entry : Mesh.Settings)
			{
				const FValuePtr& Value = Entry.Value;
				if (Seen.Contains(Entry.Name))
				{
					Diagnostics.Error(TEXT("DFX3060"), Entry.Location,
						FString::Printf(TEXT("DynamicMesh '%s' sets '%s' twice."), *Mesh.Name, *Entry.Name));
					bOk = false;
					continue;
				}
				Seen.Add(Entry.Name);

				auto Shape = [&](const TCHAR* Expected)
				{
					Diagnostics.Error(TEXT("DFX3060"), Entry.Location,
						FString::Printf(TEXT("DynamicMesh setting '%s' takes %s."), *Entry.Name, Expected));
					bOk = false;
				};

				if (Entry.Name == TEXT("Material"))
				{
					bOk &= ResolveMaterial(Entry, System.Root, Options, Plan.Material, Diagnostics);
				}
				else if (Entry.Name == TEXT("TexCoords"))
				{
					if (!IsIntegerNumber(Value) || Value->Number < 0 || Value->Number > MaxTexCoords)
					{
						Shape(TEXT("an integer from 0 to 8 (UV channels per vertex)"));
						continue;
					}
					Plan.TexCoords = static_cast<int32>(Value->Number);
				}
				else if (Entry.Name == TEXT("Colors"))
				{
					if (!Value.IsValid() || Value->Kind != EValueKind::Bool)
					{
						Shape(TEXT("true or false"));
						continue;
					}
					Plan.bColors = Value->bBool;
				}
				else if (Entry.Name == TEXT("Normals"))
				{
					const FString Mode = Value.IsValid() && Value->Kind == EValueKind::Name ? Value->Text : FString();
					if (Mode == TEXT("None")) { Plan.Normals = ENormalMode::None; }
					else if (Mode == TEXT("Auto")) { Plan.Normals = ENormalMode::Auto; }
					else if (Mode == TEXT("Explicit")) { Plan.Normals = ENormalMode::Explicit; }
					else { Shape(TEXT("None, Auto or Explicit")); }
				}
				else if (Entry.Name == TEXT("Bounds"))
				{
					bool bBox = Value.IsValid() && Value->Kind == EValueKind::Call && Value->Text == TEXT("box")
						&& Value->Elements.Num() == 6 && Value->Arguments.Num() == 0;
					for (int32 Index = 0; bBox && Index < 6; ++Index)
					{
						bBox = Value->Elements[Index].IsValid() && Value->Elements[Index]->Kind == EValueKind::Number;
					}
					if (!bBox)
					{
						Shape(TEXT("box(MinX, MinY, MinZ, MaxX, MaxY, MaxZ) with six numbers"));
						continue;
					}
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						Plan.BoundsMin[Axis] = Value->Elements[Axis]->Number;
						Plan.BoundsMax[Axis] = Value->Elements[Axis + 3]->Number;
						if (Plan.BoundsMin[Axis] > Plan.BoundsMax[Axis])
						{
							Shape(TEXT("a box whose minimum is not above its maximum on any axis"));
							break;
						}
					}
					Plan.Bounds = Value;
				}
				else if (Entry.Name == TEXT("Duration"))
				{
					if (!Value.IsValid() || Value->Kind != EValueKind::Number || Value->Number <= 0.0)
					{
						Shape(TEXT("a positive number of seconds"));
						continue;
					}
					Plan.Duration = Value->Number;
				}
				else if (Entry.Name == TEXT("Visible"))
				{
					Plan.Visible = Value;
					Plan.VisibleLocation = Entry.Location;
				}
				else
				{
					Diagnostics.Error(TEXT("DFX3060"), Entry.Location, FString::Printf(
						TEXT("Unknown DynamicMesh setting '%s'. Expected Material, TexCoords, Colors, Normals, Bounds, Duration or Visible."),
						*Entry.Name));
					bOk = false;
				}
			}

			if (!Plan.Bounds.IsValid())
			{
				FValuePtr Box = FValue::Make(EValueKind::Call, Mesh.Location);
				Box->Text = TEXT("box");
				for (const double Component : { -1000.0, -1000.0, -1000.0, 1000.0, 1000.0, 1000.0 })
				{
					Box->Elements.Add(MakeFloat(Component, Mesh.Location));
				}
				Plan.Bounds = Box;
			}
			return bOk;
		}

		bool ReadPatch(const FDocument& System, const FDynamicMesh& Mesh, const FDynamicMeshPatch& Patch,
			const FDynamicMeshLowerOptions& Options, FPatchPlan& Plan, FDiagnosticSink& Diagnostics)
		{
			const FValue& Call = *Patch.Topology;
			Plan.Source = &Patch;

			const FTopologyInfo* Info = nullptr;
			for (const FTopologyInfo& Candidate : Topologies)
			{
				if (Call.Text == Candidate.Name)
				{
					Info = &Candidate;
				}
			}
			if (Info == nullptr)
			{
				Diagnostics.Error(TEXT("DFX3058"), Call.Location, FString::Printf(
					TEXT("Patch '%s': unknown topology '%s'. Expected Grid(Columns, Rows), Strip(Columns), Ring(Columns, Rows), Tube(Columns, Rows) or Sphere(Columns, Rows)."),
					*Patch.Name, *Call.Text));
				return false;
			}
			Plan.Kind = Info->Kind;
			Plan.KindName = Info->Name;

			if (Call.Elements.Num() != Info->Counts)
			{
				Diagnostics.Error(TEXT("DFX3058"), Call.Location, FString::Printf(
					TEXT("Patch '%s': %s takes %d vertex count(s) before its options, and got %d."),
					*Patch.Name, Info->Name, Info->Counts, Call.Elements.Num()));
				return false;
			}
			int32 Counts[2] = { 2, 2 };
			for (int32 Index = 0; Index < Info->Counts; ++Index)
			{
				const FValuePtr& Count = Call.Elements[Index];
				if (!IsIntegerNumber(Count) || Count->Number < 2 || Count->Number > MaxPatchVertices)
				{
					Diagnostics.Error(TEXT("DFX3058"), Count.IsValid() ? Count->Location : Call.Location, FString::Printf(
						TEXT("Patch '%s': vertex counts are integer literals of at least 2 (a grid needs two columns and two rows to hold a quad)."),
						*Patch.Name));
					return false;
				}
				Counts[Index] = static_cast<int32>(Count->Number);
			}
			Plan.Columns = Counts[0];
			Plan.Rows = Info->Counts == 1 ? 2 : Counts[1];

			bool bOk = true;
			TSet<FString> Seen;
			for (const FNamedArgument& Argument : Call.Arguments)
			{
				if (Seen.Contains(Argument.Name))
				{
					Diagnostics.Error(TEXT("DFX3058"), Argument.Location,
						FString::Printf(TEXT("Patch '%s' sets '%s' twice."), *Patch.Name, *Argument.Name));
					bOk = false;
					continue;
				}
				Seen.Add(Argument.Name);

				if (Argument.Name == TEXT("Copies"))
				{
					if (!IsIntegerNumber(Argument.Value) || Argument.Value->Number < 1 || Argument.Value->Number > MaxPatchVertices)
					{
						Diagnostics.Error(TEXT("DFX3058"), Argument.Location,
							FString::Printf(TEXT("Patch '%s': Copies takes an integer literal of at least 1."), *Patch.Name));
						bOk = false;
						continue;
					}
					Plan.Copies = static_cast<int32>(Argument.Value->Number);
				}
				else if (Argument.Name == TEXT("Flip"))
				{
					if (!Argument.Value.IsValid() || Argument.Value->Kind != EValueKind::Bool)
					{
						Diagnostics.Error(TEXT("DFX3058"), Argument.Location,
							FString::Printf(TEXT("Patch '%s': Flip takes true or false."), *Patch.Name));
						bOk = false;
						continue;
					}
					Plan.bFlip = Argument.Value->bBool;
				}
				else if (Argument.Name == TEXT("Material"))
				{
					FPropertyEntry Entry;
					Entry.Name = Argument.Name;
					Entry.Value = Argument.Value;
					Entry.Location = Argument.Location;
					bOk &= ResolveMaterial(Entry, System.Root, Options, Plan.Material, Diagnostics);
					Plan.bHasMaterial = true;
				}
				else
				{
					Diagnostics.Error(TEXT("DFX3058"), Argument.Location, FString::Printf(
						TEXT("Patch '%s': unknown option '%s'. Expected Copies, Material or Flip."), *Patch.Name, *Argument.Name));
					bOk = false;
				}
			}

			Plan.VertexCount = int64(Plan.Columns) * Plan.Rows * Plan.Copies;
			Plan.TriangleCount = int64(2) * (Plan.Columns - 1) * (Plan.Rows - 1) * Plan.Copies;
			if (Plan.VertexCount > MaxPatchVertices)
			{
				Diagnostics.Error(TEXT("DFX3064"), Call.Location, FString::Printf(
					TEXT("Patch '%s' has %lld vertices (%d x %d x %d copies); a patch holds at most %lld, because the generated index arithmetic is exact in float only that far. Split it into more patches."),
					*Patch.Name, Plan.VertexCount, Plan.Columns, Plan.Rows, Plan.Copies, MaxPatchVertices));
				bOk = false;
			}
			// An empty body is allowed: the topology's default shape is a useful first render.
			return bOk;
		}

		/** Strips `//` and block comments so a commented-out write is not taken for one. */
		FString StripComments(const FString& Text)
		{
			FString Result;
			Result.Reserve(Text.Len());
			for (int32 Index = 0; Index < Text.Len();)
			{
				if (Text[Index] == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/'))
				{
					while (Index < Text.Len() && Text[Index] != TEXT('\n')) { ++Index; }
					continue;
				}
				if (Text[Index] == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('*'))
				{
					Index += 2;
					while (Index + 1 < Text.Len() && !(Text[Index] == TEXT('*') && Text[Index + 1] == TEXT('/'))) { ++Index; }
					Index += 2;
					Result.AppendChar(TEXT(' '));
					continue;
				}
				Result.AppendChar(Text[Index++]);
			}
			return Result;
		}

		/** The first particle attribute the body writes, or empty. Token-level, like the module generator's. */
		FString FindParticleWrite(const FString& Body)
		{
			const FString Text = StripComments(Body);
			auto IsIdent = [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); };
			for (int32 Index = Text.Find(TEXT("Particles."), ESearchCase::CaseSensitive); Index != INDEX_NONE;
				Index = Text.Find(TEXT("Particles."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1))
			{
				if (Index > 0 && (IsIdent(Text[Index - 1]) || Text[Index - 1] == TEXT('.')))
				{
					continue;
				}
				int32 End = Index + 10;
				while (End < Text.Len() && (IsIdent(Text[End]) || Text[End] == TEXT('.'))) { ++End; }
				const FString Name = Text.Mid(Index, End - Index);

				int32 Before = Index - 1;
				while (Before >= 0 && FChar::IsWhitespace(Text[Before])) { --Before; }
				if (Before >= 1 && ((Text[Before] == TEXT('+') && Text[Before - 1] == TEXT('+'))
					|| (Text[Before] == TEXT('-') && Text[Before - 1] == TEXT('-'))))
				{
					return Name;
				}

				int32 After = End;
				while (After < Text.Len() && FChar::IsWhitespace(Text[After])) { ++After; }
				if (After >= Text.Len()) { continue; }
				const TCHAR C0 = Text[After];
				const TCHAR C1 = After + 1 < Text.Len() ? Text[After + 1] : TEXT('\0');
				const TCHAR C2 = After + 2 < Text.Len() ? Text[After + 2] : TEXT('\0');
				if (C0 == TEXT('=') && C1 != TEXT('=')) { return Name; }
				if (C1 == TEXT('=') && FCString::Strchr(TEXT("+-*/%&|^"), C0) != nullptr) { return Name; }
				if ((C0 == TEXT('+') && C1 == TEXT('+')) || (C0 == TEXT('-') && C1 == TEXT('-'))) { return Name; }
				if (((C0 == TEXT('<') && C1 == TEXT('<')) || (C0 == TEXT('>') && C1 == TEXT('>'))) && C2 == TEXT('=')) { return Name; }
			}
			return FString();
		}

		// ----------------------------------------------------------------------- generated modules

		FString ModuleHeader(const FDocument& System, const FString& AssetName, const FString& What)
		{
			return FString::Printf(
				TEXT("// Generated by DreamFX from %s: %s.\n")
				TEXT("// Do not edit -- edit the DynamicMesh block in the .dfs; the next build rewrites this file.\n")
				TEXT("Module(Name=\"%s\", Root=\"%s\")\n{\n"),
				*FPaths::GetCleanFilename(System.SourceFilePath), *What,
				*ModuleRelativeName(System, AssetName), *System.Root);
		}

		FString BuildTopologyModule(const FDocument& System)
		{
			FString Text = ModuleHeader(System, TopologyModuleName, TEXT("the grid topology every DynamicMesh patch shares"));
			Text += TEXT(R"(    Settings = {
        Usage       = ParticleSpawn;
        Category    = "DreamFX|DynamicMesh";
        Description = "DynamicMesh: stores this particle's vertex index and writes the two triangles of the grid quad whose first corner it is.";
    }

    Inputs = {
        DI<DynamicMesh> DM_Mesh;
        int  DM_Columns      = 2;
        int  DM_Rows         = 2;
        int  DM_VertexBase   = 0;
        int  DM_TriangleBase = 0;
        bool DM_Flip         = false;
    }

    Body = {
        // One particle per vertex, numbered copy by copy, row by row, column by column.
        int Particles.DynamicMesh.Vertex = ExecIndex();
        float DM_Local = (float)Particles.DynamicMesh.Vertex;
        float DM_NU = (float)DM_Columns;
        float DM_NV = (float)DM_Rows;
        float DM_PerCopy = DM_NU * DM_NV;
        float DM_K = floor((DM_Local + 0.5) / DM_PerCopy);
        float DM_InCopy = DM_Local - DM_K * DM_PerCopy;
        float DM_J = floor((DM_InCopy + 0.5) / DM_NU);
        float DM_I = DM_InCopy - DM_J * DM_NU;

        // The quad to the +U +V side of this vertex; the last column and row have none.
        bool DM_HasQuad = (DM_I < DM_NU - 1.5) && (DM_J < DM_NV - 1.5);
        float DM_Quad = DM_K * (DM_NU - 1.0) * (DM_NV - 1.0) + DM_J * (DM_NU - 1.0) + DM_I;
        int DM_Triangle = DM_TriangleBase + 2 * (int)(DM_Quad + 0.5);
        int DM_V00 = DM_VertexBase + Particles.DynamicMesh.Vertex;
        int DM_V10 = DM_V00 + 1;
        int DM_V01 = DM_V00 + DM_Columns;
        int DM_V11 = DM_V01 + 1;

        // Front face = the side cross(dP/dU, dP/dV) points to: seen from there, the renderer culls
        // (V00, V10, V01), so the unflipped order is (V00, V01, V10) -- measured with a one-sided
        // material, not derived. Flip turns both triangles around.
        int DM_A = DM_Flip ? DM_V10 : DM_V01;
        int DM_B = DM_Flip ? DM_V01 : DM_V10;
        int DM_C = DM_Flip ? DM_V11 : DM_V01;
        int DM_D = DM_Flip ? DM_V01 : DM_V11;
        DM_Mesh.SetTriangleVertices(DM_HasQuad, DM_Triangle, DM_V00, DM_A, DM_B);
        DM_Mesh.SetTriangleVertices(DM_HasQuad, DM_Triangle + 1, DM_V10, DM_C, DM_D);
    }
}
)");
			return Text;
		}

		FString BuildVisibilityModule(const FDocument& System)
		{
			FString Text = ModuleHeader(System, VisibilityModuleName, TEXT("the Visible setting of DynamicMesh blocks"));
			Text += TEXT(R"(    Settings = {
        Usage       = [ParticleSpawn, ParticleUpdate];
        Category    = "DreamFX|DynamicMesh";
        Description = "DynamicMesh: hides the mesh while Visible is at most 0.5, through the renderer visibility tag (the mesh renderer draws tag 0).";
    }

    Inputs = {
        float DM_Visible = 1.0;
    }

    Body = {
        int Particles.VisibilityTag = DM_Visible > 0.5 ? 0 : 1;
    }
}
)");
			return Text;
		}

		/** The body of one evaluation of a patch: coordinates, helpers, default outputs, then the author's code. */
		FString Evaluation(const FMeshPlan& Mesh, const FPatchPlan& Patch, const FString& UExpr, const FString& VExpr,
			const FString& IExpr, const FString& JExpr, const TCHAR* Indent)
		{
			TArray<FString> Lines;
			Lines.Add(FString::Printf(TEXT("float U = %s;"), *UExpr));
			Lines.Add(FString::Printf(TEXT("float V = %s;"), *VExpr));
			Lines.Add(FString::Printf(TEXT("float I = %s;"), *IExpr));
			Lines.Add(FString::Printf(TEXT("float J = %s;"), *JExpr));

			FString DefaultPosition;
			FString DefaultNormal;
			FString DefaultTangent = TEXT("float3(1.0, 0.0, 0.0)");
			switch (Patch.Kind)
			{
			case ETopology::Grid:
				DefaultPosition = TEXT("float3((U - 0.5) * 100.0, (V - 0.5) * 100.0, 0.0)");
				DefaultNormal = TEXT("float3(0.0, 0.0, 1.0)");
				break;
			case ETopology::Strip:
				DefaultPosition = TEXT("float3((U - 0.5) * 100.0, (V - 0.5) * 10.0, 0.0)");
				DefaultNormal = TEXT("float3(0.0, 0.0, 1.0)");
				break;
			case ETopology::Ring:
				Lines.Add(TEXT("float Theta = U * 6.28318531;"));
				Lines.Add(TEXT("float3 Dir = float3(cos(Theta), sin(Theta), 0.0);"));
				DefaultPosition = TEXT("Dir * lerp(50.0, 100.0, V)");
				DefaultNormal = TEXT("float3(0.0, 0.0, -1.0)");
				DefaultTangent = TEXT("float3(-sin(Theta), cos(Theta), 0.0)");
				break;
			case ETopology::Tube:
				Lines.Add(TEXT("float Theta = U * 6.28318531;"));
				Lines.Add(TEXT("float3 Dir = float3(cos(Theta), sin(Theta), 0.0);"));
				DefaultPosition = TEXT("Dir * 50.0 + float3(0.0, 0.0, V * 100.0)");
				DefaultNormal = TEXT("Dir");
				DefaultTangent = TEXT("float3(-sin(Theta), cos(Theta), 0.0)");
				break;
			case ETopology::Sphere:
				Lines.Add(TEXT("float Theta = U * 6.28318531;"));
				Lines.Add(TEXT("float Phi = (1.0 - V) * 3.14159265;"));
				Lines.Add(TEXT("float3 Dir = float3(sin(Phi) * cos(Theta), sin(Phi) * sin(Theta), cos(Phi));"));
				DefaultPosition = TEXT("Dir * 100.0");
				DefaultNormal = TEXT("Dir");
				DefaultTangent = TEXT("float3(-sin(Theta), cos(Theta), 0.0)");
				break;
			}
			if (Patch.bFlip)
			{
				DefaultNormal = FString::Printf(TEXT("-(%s)"), *DefaultNormal);
			}

			Lines.Add(FString::Printf(TEXT("float3 Position = %s;"), *DefaultPosition));
			Lines.Add(TEXT("float4 Color = float4(1.0, 1.0, 1.0, 1.0);"));
			for (int32 Channel = 0; Channel < Mesh.TexCoords; ++Channel)
			{
				Lines.Add(Channel == 0
					? FString(TEXT("float2 UV0 = float2(U, V);"))
					: FString::Printf(TEXT("float2 UV%d = float2(0.0, 0.0);"), Channel));
			}
			Lines.Add(FString::Printf(TEXT("float3 Normal = %s;"), *DefaultNormal));
			Lines.Add(FString::Printf(TEXT("float3 Tangent = %s;"), *DefaultTangent));

			FString Text;
			for (const FString& Line : Lines)
			{
				Text += Indent;
				Text += Line;
				Text += TEXT("\n");
			}
			Text += Indent;
			// The newline is not cosmetic: a body written on one line (`{ Position = ...; }`) would
			// otherwise land inside this comment, and the patch would quietly draw its default shape.
			Text += FString::Printf(TEXT("// ---- patch %s, as written in the .dfs ----\n"), *Patch.Source->Name);
			Text += Patch.Source->Body;
			Text += TEXT("\n");
			Text += Indent;
			Text += TEXT("// ---- end of patch body ----\n");
			return Text;
		}

		FString BuildPatchModule(const FDocument& System, const FDynamicMesh& Source, const FMeshPlan& Mesh,
			const FPatchPlan& Patch, const FString& ModuleAssetName)
		{
			const FString What = FString::Printf(TEXT("DynamicMesh %s, patch %s (%s %d x %d, %d cop%s)"),
				*Source.Name, *Patch.Source->Name, *Patch.KindName, Patch.Columns, Patch.Rows, Patch.Copies,
				Patch.Copies == 1 ? TEXT("y") : TEXT("ies"));
			FString Text = ModuleHeader(System, ModuleAssetName, What);
			Text += FString::Printf(TEXT(R"(    Settings = {
        Usage       = [ParticleSpawn, ParticleUpdate];
        Category    = "DreamFX|DynamicMesh";
        Description = "%s: computes this particle's vertex and writes it into the mesh.";
    }

    Inputs = {
        DI<DynamicMesh> DM_Mesh;
        float DM_Time = 0.0;
)"), *QuoteHlslString(What));
			for (const FParameterDecl& Input : Source.Inputs)
			{
				const FString Type = Input.InnerTypeName.IsEmpty()
					? Input.TypeName
					: FString::Printf(TEXT("%s<%s>"), *Input.TypeName, *Input.InnerTypeName);
				Text += FString::Printf(TEXT("        %s %s;\n"), *Type, *Input.Name);
			}
			Text += TEXT("    }\n\n    Body = {\n");

			const TCHAR* Top = TEXT("        ");
			const TCHAR* Nested = TEXT("            ");
			const int64 PerCopy = int64(Patch.Columns) * Patch.Rows;
			const uint32 Seed = FCrc::StrCrc32(*FString::Printf(TEXT("%s/%s"), *Source.Name, *Patch.Source->Name)) % 997u;

			TArray<FString> Prologue;
			Prologue.Add(TEXT("// Which vertex this particle is: copy K, row DM_J, column DM_I."));
			Prologue.Add(FString::Printf(TEXT("int %s;"), VertexAttribute));
			Prologue.Add(FString::Printf(TEXT("float DM_Local = (float)%s;"), VertexAttribute));
			Prologue.Add(FString::Printf(TEXT("float NU = %s;"), *FormatCount(Patch.Columns)));
			Prologue.Add(FString::Printf(TEXT("float NV = %s;"), *FormatCount(Patch.Rows)));
			Prologue.Add(FString::Printf(TEXT("float NK = %s;"), *FormatCount(Patch.Copies)));
			Prologue.Add(FString::Printf(TEXT("float K = floor((DM_Local + 0.5) / %s);"), *FormatCount(PerCopy)));
			Prologue.Add(FString::Printf(TEXT("float DM_InCopy = DM_Local - K * %s;"), *FormatCount(PerCopy)));
			Prologue.Add(TEXT("float DM_J = floor((DM_InCopy + 0.5) / NU);"));
			Prologue.Add(TEXT("float DM_I = DM_InCopy - DM_J * NU;"));
			Prologue.Add(TEXT("float T = DM_Time;"));
			Prologue.Add(FString::Printf(TEXT("float Index = %s + DM_Local;"), *FormatCount(Patch.VertexBase)));
			Prologue.Add(FString::Printf(TEXT("float Rand = frac(sin((K + %s) * 12.9898) * 43758.5453);"), *FormatFloat(Seed * 0.731)));
			for (const FString& Line : Prologue)
			{
				Text += Top + Line + TEXT("\n");
			}

			const FString BaseU = TEXT("DM_I / (NU - 1.0)");
			const FString BaseV = TEXT("DM_J / (NV - 1.0)");
			if (Mesh.Normals == ENormalMode::Auto)
			{
				// Three extra evaluations a quarter step away, always towards the inside of the grid so an
				// open edge never extrapolates and a pole always has a non-degenerate neighbour. They come
				// before the main evaluation, so their locals shadow nothing.
				Text += TEXT("\n");
				Text += Top + FString(TEXT("// Normals = Auto: the body evaluated a quarter grid step away in U, in V, and in both.\n"));
				Text += Top + FString(TEXT("float DM_SU = (DM_I < 0.5 * (NU - 1.0) ? 0.25 : -0.25) / (NU - 1.0);\n"));
				Text += Top + FString(TEXT("float DM_SV = (DM_J < 0.5 * (NV - 1.0) ? 0.25 : -0.25) / (NV - 1.0);\n"));
				Text += Top + FString(TEXT("float3 DM_PU;\n"));
				Text += Top + FString(TEXT("float3 DM_PV;\n"));
				Text += Top + FString(TEXT("float3 DM_PUV;\n"));
				struct FSample { const TCHAR* Target; bool bU; bool bV; };
				for (const FSample& Sample : { FSample{ TEXT("DM_PU"), true, false }, FSample{ TEXT("DM_PV"), false, true },
					FSample{ TEXT("DM_PUV"), true, true } })
				{
					Text += Top + FString(TEXT("{\n"));
					Text += Evaluation(Mesh, Patch,
						Sample.bU ? BaseU + TEXT(" + DM_SU") : BaseU,
						Sample.bV ? BaseV + TEXT(" + DM_SV") : BaseV,
						Sample.bU ? FString(TEXT("DM_I + DM_SU * (NU - 1.0)")) : FString(TEXT("DM_I")),
						Sample.bV ? FString(TEXT("DM_J + DM_SV * (NV - 1.0)")) : FString(TEXT("DM_J")),
						Nested);
					Text += Nested + FString::Printf(TEXT("%s = Position;\n"), Sample.Target);
					Text += Top + FString(TEXT("}\n"));
				}
			}

			Text += TEXT("\n");
			Text += Evaluation(Mesh, Patch, BaseU, BaseV, TEXT("DM_I"), TEXT("DM_J"), Top);
			Text += TEXT("\n");

			if (Mesh.Normals == ENormalMode::Auto)
			{
				Text += Top + FString(TEXT("float3 DM_Du = ((DM_PU - Position) + (DM_PUV - DM_PV)) / DM_SU;\n"));
				Text += Top + FString(TEXT("float3 DM_Dv = ((DM_PV - Position) + (DM_PUV - DM_PU)) / DM_SV;\n"));
				Text += Top + FString::Printf(TEXT("float3 DM_N = %s;\n"),
					Patch.bFlip ? TEXT("cross(DM_Dv, DM_Du)") : TEXT("cross(DM_Du, DM_Dv)"));
				Text += Top + FString(TEXT("DM_N = DM_N * rsqrt(max(dot(DM_N, DM_N), 1e-20));\n"));
				Text += Top + FString(TEXT("float3 DM_T = DM_Du * rsqrt(max(dot(DM_Du, DM_Du), 1e-20));\n"));
			}
			else if (Mesh.Normals == ENormalMode::Explicit)
			{
				Text += Top + FString(TEXT("float3 DM_N = Normal * rsqrt(max(dot(Normal, Normal), 1e-20));\n"));
				Text += Top + FString(TEXT("float3 DM_T = Tangent - DM_N * dot(DM_N, Tangent);\n"));
				Text += Top + FString(TEXT("DM_T = DM_T * rsqrt(max(dot(DM_T, DM_T), 1e-20));\n"));
			}

			Text += Top + FString(TEXT("// Write this vertex.\n"));
			Text += Top + FString::Printf(TEXT("int DM_Vertex = %lld + %s;\n"), Patch.VertexBase, VertexAttribute);
			Text += Top + FString(TEXT("DM_Mesh.SetVertexPosition(true, DM_Vertex, Position);\n"));
			if (Mesh.bColors)
			{
				Text += Top + FString(TEXT("DM_Mesh.SetVertexColor(true, DM_Vertex, Color);\n"));
			}
			if (Mesh.Normals != ENormalMode::None)
			{
				// The data interface stores the binormal sign the opposite way round from what the mesh
				// vertex factory reconstructs (cross(Z, X) * w), so passing cross(T, N) is what makes the
				// shader's binormal come out as cross(N, T) -- +V on an unflipped grid.
				Text += Top + FString(TEXT("DM_Mesh.SetVertexTangentBasis(true, DM_Vertex, DM_T, cross(DM_T, DM_N), DM_N);\n"));
			}
			if (Mesh.TexCoords > 0)
			{
				const FString UvVertex = Mesh.TexCoordStride > 1
					? FString::Printf(TEXT("DM_Vertex * %d"), Mesh.TexCoordStride)
					: FString(TEXT("DM_Vertex"));
				if (Mesh.TexCoordStride > 1)
				{
					Text += Top + FString::Printf(TEXT(
						"// UE 5.8: SetVertexTexCoord addresses Vertex*2 + TexCoord*2 while the renderer reads\n"
						"%s// (Vertex*NumTexCoords + TexCoord)*2, so the vertex index is scaled by the channel count\n"
						"%s// (and the data interface declares that many times the vertices, for the bounds check).\n"), Top, Top);
				}
				for (int32 Channel = 0; Channel < Mesh.TexCoords; ++Channel)
				{
					Text += Top + FString::Printf(TEXT("DM_Mesh.SetVertexTexCoord(true, %s, %d, UV%d);\n"), *UvVertex, Channel, Channel);
				}
			}
			Text += TEXT("    }\n}\n");
			return Text;
		}

		// ------------------------------------------------------------------- data interface JSON

		FString BuildMeshJson(const FMeshPlan& Mesh)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sections;
			TArray<TSharedPtr<FJsonValue>> Materials;
			for (int32 Section = 0; Section < Mesh.SectionMaterials.Num(); ++Section)
			{
				TSharedRef<FJsonObject> SectionObject = MakeShared<FJsonObject>();
				SectionObject->SetNumberField(TEXT("NumTriangles"), static_cast<double>(Mesh.SectionTriangles[Section]));
				SectionObject->SetNumberField(TEXT("MaterialIndex"), Section);
				Sections.Add(MakeShared<FJsonValueObject>(SectionObject));

				TSharedRef<FJsonObject> MaterialObject = MakeShared<FJsonObject>();
				MaterialObject->SetStringField(TEXT("Material"),
					Mesh.SectionMaterials[Section].IsEmpty() ? FString(TEXT("None")) : Mesh.SectionMaterials[Section]);
				Materials.Add(MakeShared<FJsonValueObject>(MaterialObject));
			}
			Root->SetArrayField(TEXT("Sections"), Sections);
			Root->SetArrayField(TEXT("Materials"), Materials);
			Root->SetNumberField(TEXT("NumVertices"), static_cast<double>(Mesh.TotalVertices * Mesh.TexCoordStride));
			Root->SetNumberField(TEXT("NumTexCoords"), Mesh.TexCoords);
			Root->SetBoolField(TEXT("bHasColors"), Mesh.bColors);
			Root->SetBoolField(TEXT("bHasTangentBasis"), Mesh.Normals != ENormalMode::None);
			Root->SetBoolField(TEXT("bClearTrianglesPerFrame"), false);

			auto Vector = [](const double (&Values)[3])
			{
				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetNumberField(TEXT("X"), Values[0]);
				Object->SetNumberField(TEXT("Y"), Values[1]);
				Object->SetNumberField(TEXT("Z"), Values[2]);
				return Object;
			};
			TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
			Bounds->SetObjectField(TEXT("Min"), Vector(Mesh.BoundsMin));
			Bounds->SetObjectField(TEXT("Max"), Vector(Mesh.BoundsMax));
			Bounds->SetBoolField(TEXT("IsValid"), true);
			Root->SetObjectField(TEXT("DefaultBounds"), Bounds);

			FString Json;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
			FJsonSerializer::Serialize(Root, Writer);
			return Json;
		}

		bool ParseGeneratedModule(const FDocument& System, const FString& Text, const FString& Path,
			const FSourceLocation& HeaderLocation, const FSourceLocation& BodyLocation, FDocument& OutModule,
			FDiagnosticSink& Diagnostics)
		{
			FDiagnosticSink ParseDiagnostics;
			if (!FParser::ParseText(Text, Path, OutModule, ParseDiagnostics))
			{
				// The text is the generator's, so a parse failure is a DreamFX defect, not the author's --
				// but it is reported at the block, with the parser's own words, and the text is on disk.
				Diagnostics.Error(TEXT("DFX5037"), HeaderLocation, FString::Printf(
					TEXT("The module DreamFX generated for this DynamicMesh did not parse (%s is the text). This is a DreamFX defect -- report it with the .dfs:\n%s"),
					*Path, *ParseDiagnostics.FormatAll()));
				return false;
			}
			// Provenance belongs to the .dfs; positions point at the block the author wrote.
			OutModule.SourceFilePath = System.SourceFilePath;
			OutModule.HeaderLocation = HeaderLocation;
			OutModule.BodyLocation = BodyLocation;
			for (FPropertyEntry& Setting : OutModule.Settings)
			{
				Setting.Location = HeaderLocation;
				Setting.SourceFile = System.SourceFilePath;
			}
			for (FParameterDecl& Parameter : OutModule.Parameters)
			{
				Parameter.Location = HeaderLocation;
				Parameter.SourceFile = System.SourceFilePath;
			}
			return true;
		}

		void WriteIfChanged(const FString& Path, const FString& Text)
		{
			FString Existing;
			if (FFileHelper::LoadFileToString(Existing, *Path) && Existing == Text)
			{
				return;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}

	bool DynamicMeshTexCoordsNeedVertexStride()
	{
		static const bool bNeedsStride = []()
		{
			// The defect is in the data interface's shader template (GPU) and mirrored in its VM functions
			// (CPU). The template is what a fix would touch first, and reading it is the one check that
			// follows the engine rather than a version number.
			if (const TSharedPtr<IPlugin> Niagara = IPluginManager::Get().FindPlugin(TEXT("Niagara")))
			{
				const FString File = Niagara->GetBaseDir() / TEXT("Shaders/Private/NiagaraDataInterfaceDynamicMeshTemplate.ush");
				FString Template;
				if (FFileHelper::LoadFileToString(Template, *File))
				{
					const int32 At = Template.Find(TEXT("_GetTexCoordOffsetUnsafe(int Vertex, int TexCoord)"));
					if (At != INDEX_NONE)
					{
						int32 LineEnd = Template.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
						if (LineEnd == INDEX_NONE) { LineEnd = Template.Len(); }
						const FString Line = Template.Mid(At, LineEnd - At);
						const bool bBuggy = !Line.Contains(TEXT("NumTexCoords"));
						UE_LOG(LogDreamFX, Verbose, TEXT("DynamicMesh: texture coordinate layout probe read '%s': %s."),
							*File, bBuggy ? TEXT("Vertex*2 + TexCoord*2, applying the stride workaround") : TEXT("interleaved, writing plainly"));
						return bBuggy;
					}
				}
			}
			// Unreadable or reshaped: assume the engine this was written against.
			return true;
		}();
		return bNeedsStride;
	}

	bool LowerDynamicMeshes(const FDocument& System, const FDynamicMeshLowerOptions& LowerOptions,
		FDynamicMeshLowering& OutLowering, FDiagnosticSink& Diagnostics)
	{
		OutLowering = FDynamicMeshLowering();
		OutLowering.System = System;
		OutLowering.System.DynamicMeshes.Reset();
		if (System.DynamicMeshes.IsEmpty())
		{
			return true;
		}

		bool bOk = true;
		const FString& File = System.SourceFilePath;

		// -- names: the user parameter and every emitter a block adds must be free ----------------
		TSet<FName> Parameters;
		for (const FParameterDecl& Parameter : System.Parameters)
		{
			Parameters.Add(FName(*Parameter.Name));
		}
		TMap<FName, FString> Emitters;
		for (const FEmitter& Emitter : System.Emitters)
		{
			Emitters.Add(FName(*Emitter.Name), FString(TEXT("an Emitter block")));
		}
		auto ClaimEmitter = [&](const FString& Name, const FString& Owner, const FSourceLocation& Location)
		{
			if (const FString* Existing = Emitters.Find(FName(*Name)))
			{
				Diagnostics.Error(TEXT("DFX3059"), Location, FString::Printf(
					TEXT("%s needs an emitter named '%s', which %s already uses. Rename one of them."),
					*Owner, *Name, **Existing));
				return false;
			}
			Emitters.Add(FName(*Name), Owner);
			return true;
		};

		TArray<FMeshPlan> Plans;
		for (const FDynamicMesh& Mesh : System.DynamicMeshes)
		{
			FMeshPlan& Plan = Plans.AddDefaulted_GetRef();
			Plan.Source = &Mesh;
			const FString Owner = FString::Printf(TEXT("DynamicMesh '%s'"), *Mesh.Name);
			if (!IsPlainName(Mesh.Name))
			{
				Diagnostics.Error(TEXT("DFX3066"), Mesh.Location, FString::Printf(
					TEXT("DynamicMesh '%s' needs a plain name (letters, digits, underscores): it becomes a user parameter, emitter names and generated asset names."),
					*Mesh.Name));
				bOk = false;
			}

			if (Parameters.Contains(FName(*Mesh.Name)))
			{
				Diagnostics.Error(TEXT("DFX3059"), Mesh.Location, FString::Printf(
					TEXT("DynamicMesh '%s' becomes the user parameter 'User.%s', which Properties already declares. Rename one of them."),
					*Mesh.Name, *Mesh.Name));
				bOk = false;
			}
			Parameters.Add(FName(*Mesh.Name));
			bOk &= ClaimEmitter(Mesh.Name, Owner, Mesh.Location);

			bOk &= ReadMeshSettings(System, Mesh, LowerOptions, Plan, Diagnostics);
			Plan.TexCoordStride = Plan.TexCoords > 1 && DynamicMeshTexCoordsNeedVertexStride() ? Plan.TexCoords : 1;

			// -- inputs ---------------------------------------------------------------------------
			TSet<FString> InputNames;
			for (const FParameterDecl& Input : Mesh.Inputs)
			{
				bool bReserved = Input.Name.StartsWith(TEXT("DM_"));
				for (const TCHAR* Reserved : ReservedNames)
				{
					bReserved |= Input.Name.Equals(Reserved, ESearchCase::CaseSensitive);
				}
				if (bReserved)
				{
					Diagnostics.Error(TEXT("DFX3062"), Input.Location, FString::Printf(
						TEXT("Input '%s' of DynamicMesh '%s' uses a name the patch prologue already declares (U, V, I, J, NU, NV, K, NK, Rand, T, Index, Theta, Phi, Dir, the outputs, or anything starting with DM_). Rename it."),
						*Input.Name, *Mesh.Name));
					bOk = false;
				}
				if (InputNames.Contains(Input.Name))
				{
					Diagnostics.Error(TEXT("DFX3062"), Input.Location,
						FString::Printf(TEXT("Input '%s' of DynamicMesh '%s' is declared twice."), *Input.Name, *Mesh.Name));
					bOk = false;
				}
				InputNames.Add(Input.Name);
				if (!Input.DefaultValue.IsValid())
				{
					Diagnostics.Error(TEXT("DFX3062"), Input.Location, FString::Printf(
						TEXT("Input '%s' of DynamicMesh '%s' has no value. An input is passed to every patch, so it says what to pass: `%s %s = User.Something;`."),
						*Input.Name, *Mesh.Name, *Input.TypeName, *Input.Name));
					bOk = false;
				}
				FNiagaraTypeDefinition Type;
				bool bDataInterface = false;
				bOk &= FValueLowering::ResolveDeclaredType(Input, Diagnostics, Type, bDataInterface);
			}

			// -- patches ----------------------------------------------------------------------------
			if (Mesh.Patches.IsEmpty())
			{
				Diagnostics.Error(TEXT("DFX3063"), Mesh.Location, FString::Printf(
					TEXT("DynamicMesh '%s' has no Patch, so there is nothing to draw. Add one, e.g. `Patch Sheet = Grid(33, 9) { }`."),
					*Mesh.Name));
				bOk = false;
			}
			TSet<FString> PatchNames;
			for (const FDynamicMeshPatch& Patch : Mesh.Patches)
			{
				if (PatchNames.Contains(Patch.Name))
				{
					Diagnostics.Error(TEXT("DFX3059"), Patch.Location,
						FString::Printf(TEXT("DynamicMesh '%s' has two patches named '%s'."), *Mesh.Name, *Patch.Name));
					bOk = false;
					continue;
				}
				PatchNames.Add(Patch.Name);
				if (!IsPlainName(Patch.Name))
				{
					Diagnostics.Error(TEXT("DFX3066"), Patch.Location, FString::Printf(
						TEXT("Patch '%s' of DynamicMesh '%s' needs a plain name (letters, digits, underscores): it becomes an emitter name and a generated asset name."),
						*Patch.Name, *Mesh.Name));
					bOk = false;
					continue;
				}

				FPatchPlan& PatchPlan = Plan.Patches.AddDefaulted_GetRef();
				if (!ReadPatch(System, Mesh, Patch, LowerOptions, PatchPlan, Diagnostics))
				{
					bOk = false;
					continue;
				}
				PatchPlan.EmitterName = FString::Printf(TEXT("%s_%s"), *Mesh.Name, *Patch.Name);
				PatchPlan.ModuleName = FString::Printf(TEXT("DM_%s_%s"), *Mesh.Name, *Patch.Name);
				bOk &= ClaimEmitter(PatchPlan.EmitterName,
					FString::Printf(TEXT("Patch '%s' of DynamicMesh '%s'"), *Patch.Name, *Mesh.Name), Patch.Location);
				bOk &= ModuleFullPath(System, PatchPlan.ModuleName, PatchPlan.ModulePath, Diagnostics, Patch.Location);

				if (Plan.Normals == ENormalMode::Auto)
				{
					const FString Written = FindParticleWrite(Patch.Body);
					if (!Written.IsEmpty())
					{
						Diagnostics.Error(TEXT("DFX3065"), Patch.BodyLocation, FString::Printf(
							TEXT("Patch '%s' writes '%s', but DynamicMesh '%s' has Normals = Auto, which evaluates the body four times per vertex -- the write would run four times. Keep particle state out of the body, or use Normals = Explicit and write Normal yourself."),
							*Patch.Name, *Written, *Mesh.Name));
						bOk = false;
					}
				}
			}
		}

		if (!bOk)
		{
			return false;
		}

		// -- counts, sections and bases ---------------------------------------------------------
		for (FMeshPlan& Plan : Plans)
		{
			for (FPatchPlan& Patch : Plan.Patches)
			{
				const FString Material = Patch.bHasMaterial ? Patch.Material : Plan.Material;
				Patch.Section = Plan.SectionMaterials.AddUnique(Material);
				Patch.VertexBase = Plan.TotalVertices;
				Plan.TotalVertices += Patch.VertexCount;
			}
			// Sections hold contiguous triangle ranges, so triangles are laid out section by section;
			// inside a section, in patch order. That is also the draw order.
			Plan.SectionTriangles.Init(0, Plan.SectionMaterials.Num());
			for (int32 Section = 0; Section < Plan.SectionMaterials.Num(); ++Section)
			{
				for (FPatchPlan& Patch : Plan.Patches)
				{
					if (Patch.Section == Section)
					{
						Patch.TriangleBase = Plan.TotalTriangles;
						Plan.TotalTriangles += Patch.TriangleCount;
						Plan.SectionTriangles[Section] += Patch.TriangleCount;
					}
				}
			}
			if (Plan.TotalVertices * Plan.TexCoordStride > MAX_int32 || Plan.TotalTriangles * 3 > MAX_int32)
			{
				Diagnostics.Error(TEXT("DFX3064"), Plan.Source->Location, FString::Printf(
					TEXT("DynamicMesh '%s' has %lld vertices (declared %lld times %d for the texture coordinate workaround) and %lld triangles; the data interface counts them in int32. Use fewer or smaller patches, or fewer UV channels."),
					*Plan.Source->Name, Plan.TotalVertices, Plan.TotalVertices, Plan.TexCoordStride, Plan.TotalTriangles));
				return false;
			}
		}

		// -- modules ------------------------------------------------------------------------------
		FString TopologyPath;
		if (!ModuleFullPath(System, TopologyModuleName, TopologyPath, Diagnostics, System.DynamicMeshes[0].Location))
		{
			return false;
		}
		auto AddModule = [&](const FString& AssetName, const FString& Text, const FSourceLocation& Header,
			const FSourceLocation& Body)
		{
			const FString Path = DumpPath(System, AssetName);
			FDocument Module;
			if (!ParseGeneratedModule(System, Text, Path, Header, Body, Module, Diagnostics))
			{
				return false;
			}
			OutLowering.Modules.Add(MoveTemp(Module));
			OutLowering.ModuleTexts.Add(Text);
			OutLowering.ModuleDumpPaths.Add(Path);
			return true;
		};

		if (!AddModule(TopologyModuleName, BuildTopologyModule(System), System.DynamicMeshes[0].Location,
			System.DynamicMeshes[0].Location))
		{
			return false;
		}
		FString VisibilityPath;
		if (Plans.ContainsByPredicate([](const FMeshPlan& Plan) { return Plan.Visible.IsValid(); }))
		{
			if (!ModuleFullPath(System, VisibilityModuleName, VisibilityPath, Diagnostics, System.DynamicMeshes[0].Location)
				|| !AddModule(VisibilityModuleName, BuildVisibilityModule(System), System.DynamicMeshes[0].Location,
					System.DynamicMeshes[0].Location))
			{
				return false;
			}
		}

		// -- the system side -----------------------------------------------------------------------
		TArray<TPair<int32, TArray<FEmitter>>> Insertions;
		for (const FMeshPlan& Plan : Plans)
		{
			const FDynamicMesh& Mesh = *Plan.Source;
			const FSourceLocation& Location = Mesh.Location;
			const FString UserName = FString::Printf(TEXT("User.%s"), *Mesh.Name);
			const double Lifetime = Plan.Duration.Get(EndlessLifetime);
			const double LoopDuration = Plan.Duration.Get(1.0);

			FParameterDecl MeshParameter;
			MeshParameter.TypeName = TEXT("DI");
			MeshParameter.InnerTypeName = TEXT("DynamicMesh");
			MeshParameter.Name = Mesh.Name;
			MeshParameter.DefaultValue = FValue::MakeString(BuildMeshJson(Plan), Location);
			MeshParameter.Location = Location;
			MeshParameter.SourceFile = File;
			OutLowering.System.Parameters.Add(MoveTemp(MeshParameter));

			auto EmitterLifecycle = [&](FEmitter& Emitter, int64 SpawnCount)
			{
				FStack Update = MakeStack(EStackKind::EmitterUpdate, Location, File);
				Update.Statements.Add(MakeCall(EmitterStateModule, {
					MakeArgument(TEXT("LifeCycleMode"), FValue::MakeName(TEXT("Self"), Location)),
					MakeArgument(TEXT("LoopBehavior"), FValue::MakeName(TEXT("Once"), Location)),
					MakeArgument(TEXT("LoopDuration"), MakeFloat(LoopDuration, Location)) }, Location, File));
				Update.Statements.Add(MakeCall(SpawnBurstModule, {
					MakeArgument(TEXT("SpawnCount"), MakeInt(SpawnCount, Location)),
					MakeArgument(TEXT("SpawnTime"), MakeFloat(0.0, Location)) }, Location, File));
				Emitter.Stacks.Add(MoveTemp(Update));
			};

			TArray<FEmitter> Added;
			for (const FPatchPlan& Patch : Plan.Patches)
			{
				const FDynamicMeshPatch& Source = *Patch.Source;
				const FSourceLocation& PatchLocation = Source.Location;

				const FString Text = BuildPatchModule(System, Mesh, Plan, Patch, Patch.ModuleName);
				if (!AddModule(Patch.ModuleName, Text, PatchLocation, Source.BodyLocation))
				{
					return false;
				}
				// Inputs keep their own positions, so a type error lands on the line that declares it.
				FDocument& Module = OutLowering.Modules.Last();
				for (FParameterDecl& Parameter : Module.Parameters)
				{
					if (const FParameterDecl* Input = Mesh.Inputs.FindByPredicate(
						[&](const FParameterDecl& Candidate) { return Candidate.Name == Parameter.Name; }))
					{
						Parameter.Location = Input->Location;
					}
				}

				FEmitter Writer;
				Writer.Name = Patch.EmitterName;
				Writer.Location = PatchLocation;
				Writer.FromSourceFile = File;
				Writer.Settings.Add(MakeProperty(TEXT("SimTarget"), FValue::MakeName(TEXT("GPU"), PatchLocation), PatchLocation, File));
				Writer.Settings.Add(MakeProperty(TEXT("LocalSpace"), FValue::MakeBool(true, PatchLocation), PatchLocation, File));
				Writer.Settings.Add(MakeProperty(TEXT("AllocationMode"), FValue::MakeName(TEXT("Fixed"), PatchLocation), PatchLocation, File));
				Writer.Settings.Add(MakeProperty(TEXT("PreAllocationCount"), MakeInt(Patch.VertexCount, PatchLocation), PatchLocation, File));
				Writer.Settings.Add(MakeProperty(TEXT("FixedBounds"), Plan.Bounds, PatchLocation, File));
				EmitterLifecycle(Writer, Patch.VertexCount);

				auto VertexCall = [&](bool bSpawn)
				{
					TArray<FNamedArgument> Arguments;
					Arguments.Add(MakeArgument(TEXT("DM_Mesh"), FValue::MakeName(UserName, PatchLocation)));
					Arguments.Add(MakeArgument(TEXT("DM_Time"), bSpawn
						? MakeFloat(0.0, PatchLocation)
						: FValue::MakeName(TEXT("Particles.Age"), PatchLocation)));
					for (const FParameterDecl& Input : Mesh.Inputs)
					{
						FNamedArgument Argument;
						Argument.Name = Input.Name;
						Argument.Value = Input.DefaultValue;
						Argument.Location = Input.Location;
						Arguments.Add(MoveTemp(Argument));
					}
					return MakeCall(Patch.ModulePath, MoveTemp(Arguments), PatchLocation, File);
				};

				FStack Spawn = MakeStack(EStackKind::ParticleSpawn, PatchLocation, File);
				Spawn.Statements.Add(MakeCall(InitializeParticleModule, {
					MakeArgument(TEXT("LifetimeMode"), FValue::MakeName(TEXT("DirectSet"), PatchLocation)),
					MakeArgument(TEXT("Lifetime"), MakeFloat(Lifetime, PatchLocation)) }, PatchLocation, File));
				Spawn.Statements.Add(MakeCall(TopologyPath, {
					MakeArgument(TEXT("DM_Mesh"), FValue::MakeName(UserName, PatchLocation)),
					MakeArgument(TEXT("DM_Columns"), MakeInt(Patch.Columns, PatchLocation)),
					MakeArgument(TEXT("DM_Rows"), MakeInt(Patch.Rows, PatchLocation)),
					MakeArgument(TEXT("DM_VertexBase"), MakeInt(Patch.VertexBase, PatchLocation)),
					MakeArgument(TEXT("DM_TriangleBase"), MakeInt(Patch.TriangleBase, PatchLocation)),
					MakeArgument(TEXT("DM_Flip"), FValue::MakeBool(Patch.bFlip, PatchLocation)) }, PatchLocation, File));
				// A particle does not run its update stack on the frame it spawns: without this the
				// mesh's first frame would have its triangles and none of its vertices.
				Spawn.Statements.Add(VertexCall(/*bSpawn=*/true));
				Writer.Stacks.Add(MoveTemp(Spawn));

				FStack Update = MakeStack(EStackKind::ParticleUpdate, PatchLocation, File);
				Update.Statements.Add(MakeCall(ParticleStateModule, {}, PatchLocation, File));
				Update.Statements.Add(VertexCall(/*bSpawn=*/false));
				Writer.Stacks.Add(MoveTemp(Update));

				Added.Add(MoveTemp(Writer));
			}

			// -- the drawing emitter ------------------------------------------------------------------
			FEmitter Draw;
			Draw.Name = Mesh.Name;
			Draw.Location = Location;
			Draw.FromSourceFile = File;
			Draw.Settings.Add(MakeProperty(TEXT("SimTarget"), FValue::MakeName(TEXT("CPU"), Location), Location, File));
			Draw.Settings.Add(MakeProperty(TEXT("LocalSpace"), FValue::MakeBool(true, Location), Location, File));
			Draw.Settings.Add(MakeProperty(TEXT("FixedBounds"), Plan.Bounds, Location, File));
			EmitterLifecycle(Draw, 1);

			auto VisibilityCall = [&]()
			{
				FNamedArgument Argument;
				Argument.Name = TEXT("DM_Visible");
				Argument.Value = Plan.Visible;
				Argument.Location = Plan.VisibleLocation;
				return MakeCall(VisibilityPath, { Argument }, Plan.VisibleLocation, File);
			};

			FStack DrawSpawn = MakeStack(EStackKind::ParticleSpawn, Location, File);
			DrawSpawn.Statements.Add(MakeCall(InitializeParticleModule, {
				MakeArgument(TEXT("LifetimeMode"), FValue::MakeName(TEXT("DirectSet"), Location)),
				MakeArgument(TEXT("Lifetime"), MakeFloat(Lifetime, Location)),
				MakeArgument(TEXT("ColorMode"), FValue::MakeName(TEXT("DirectSet"), Location)),
				MakeArgument(TEXT("Color"), MakeVector({ 1.0, 1.0, 1.0, 1.0 }, Location)),
				MakeArgument(TEXT("PositionMode"), FValue::MakeName(TEXT("DirectSet"), Location)),
				MakeArgument(TEXT("Position"), MakeVector({ 0.0, 0.0, 0.0 }, Location)),
				MakeArgument(TEXT("MeshScaleMode"), FValue::MakeName(TEXT("NonUniform"), Location)),
				MakeArgument(TEXT("MeshScale"), MakeVector({ 1.0, 1.0, 1.0 }, Location)) }, Location, File));
			if (Plan.Visible.IsValid())
			{
				DrawSpawn.Statements.Add(VisibilityCall());
			}
			Draw.Stacks.Add(MoveTemp(DrawSpawn));

			FStack DrawUpdate = MakeStack(EStackKind::ParticleUpdate, Mesh.Draw.Location.IsValid() ? Mesh.Draw.Location : Location, File);
			DrawUpdate.Statements.Add(MakeCall(ParticleStateModule, {}, Location, File));
			if (Plan.Visible.IsValid())
			{
				DrawUpdate.Statements.Add(VisibilityCall());
			}
			DrawUpdate.Statements.Append(Mesh.Draw.Statements);
			Draw.Stacks.Add(MoveTemp(DrawUpdate));

			FRenderer Renderer;
			Renderer.TypeName = TEXT("MeshRenderer");
			Renderer.Name = Mesh.Name;
			Renderer.Location = Location;
			Renderer.SourceFile = File;
			Renderer.Properties.Add(MakeProperty(TEXT("Meshes"), MakeStringArray({ FString() }, Location), Location, File));
			Renderer.Properties.Add(MakeProperty(TEXT("MeshBindings"), MakeStringArray({ UserName }, Location), Location, File));
			for (const FPropertyEntry& Property : Mesh.RendererProperties)
			{
				if (Property.Name == TEXT("Meshes") || Property.Name == TEXT("MeshBindings"))
				{
					Diagnostics.Error(TEXT("DFX3060"), Property.Location, FString::Printf(
						TEXT("DynamicMesh '%s' sets the renderer's '%s' itself: the renderer draws this mesh and nothing else."),
						*Mesh.Name, *Property.Name));
					return false;
				}
				Renderer.Properties.Add(Property);
			}
			Draw.Renderers.Add(MoveTemp(Renderer));
			Added.Add(MoveTemp(Draw));

			Insertions.Emplace(FMath::Clamp(Mesh.EmitterIndex, 0, System.Emitters.Num()), MoveTemp(Added));
		}

		// Emitters go where the block was written; blocks at one position keep their source order.
		Insertions.StableSort([](const TPair<int32, TArray<FEmitter>>& A, const TPair<int32, TArray<FEmitter>>& B)
			{ return A.Key < B.Key; });
		int32 Shift = 0;
		for (TPair<int32, TArray<FEmitter>>& Insertion : Insertions)
		{
			const int32 At = Insertion.Key + Shift;
			OutLowering.System.Emitters.Insert(Insertion.Value, At);
			Shift += Insertion.Value.Num();
		}

		// The system's identity covers what the blocks lowered to, not only the text that holds them.
		TMap<FString, FString> Lowered;
		Lowered.Add(TEXT("dynamic-mesh:lowering"), LoweringVersion);
		for (int32 Index = 0; Index < OutLowering.Modules.Num(); ++Index)
		{
			Lowered.Add(FString::Printf(TEXT("dynamic-mesh:%s"), *OutLowering.Modules[Index].Name), OutLowering.Modules[Index].SourceHash);
		}
		OutLowering.System.SourceHash = FProvenance::HashWithSourceDependencies(System.SourceHash, Lowered);
		return true;
	}

	bool BuildDynamicMeshModules(const FDynamicMeshLowering& Lowering, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		bool bOk = true;
		for (int32 Index = 0; Index < Lowering.Modules.Num(); ++Index)
		{
			const FDocument& Module = Lowering.Modules[Index];
			const FString& DumpFile = Lowering.ModuleDumpPaths[Index];
			// Written by a build that saves, which is the build an author reads the output of; an in-memory
			// build (tests, previews) would only litter Saved/ with one copy per throw-away system.
			if (Options.bSave && !Options.bVerifyOnly)
			{
				WriteIfChanged(DumpFile, Lowering.ModuleTexts[Index]);
			}

			FDiagnosticSink ModuleDiagnostics;
			FLint::Run(Module, ModuleDiagnostics);
			if (!ModuleDiagnostics.HasErrors())
			{
				const FModuleGenerateResult Result = FModuleGenerator::IsAvailable()
					? FModuleGenerator::Generate(Module, Options, ModuleDiagnostics)
					: FModuleGenerator::CheckWithoutGenerating(Module, ModuleDiagnostics);
				bOk &= Result.bSucceeded;
			}

			// Messages about the generated text say where that text is: the body Niagara rejects is the
			// patch body wrapped in a prologue, and the line numbers in its message are the dump's.
			for (const FDiagnostic& Diagnostic : ModuleDiagnostics.GetDiagnostics())
			{
				FString Message = Diagnostic.Message;
				if (Diagnostic.Severity != EDiagnosticSeverity::Info)
				{
					Message += FString::Printf(TEXT("\n  (generated DynamicMesh module '%s'; its source is %s)"), *Module.Name, *DumpFile);
				}
				Diagnostics.SetFile(Diagnostic.File.IsEmpty() ? Module.SourceFilePath : Diagnostic.File);
				Diagnostics.Add(Diagnostic.Severity, Diagnostic.Code, Diagnostic.Location, Message);
			}
			bOk &= !ModuleDiagnostics.HasErrors();
		}
		if (!Lowering.Modules.IsEmpty())
		{
			Diagnostics.SetFile(Lowering.Modules[0].SourceFilePath);
		}
		return bOk;
	}
}

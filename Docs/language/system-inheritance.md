# System source inheritance

A `.dfs` system can inherit another `.dfs` source with `Parent`:

```cpp
// Base.dfs
System(Name="FX/NS_Base", Root="Game")
{
    Properties = { float Speed = 100.0; Color Tint = (1, 0, 0, 1); }
    Emitter Sparks from "Emitters/E_Sparks" {}
}
```

```cpp
// Variants/Blue.dfs
System(Name="FX/NS_Blue", Root="Game", Parent="../Base.dfs")
{
    Properties = { Color Tint = (0, 0, 1, 1); }
    Emitter Sparks { Settings = { LocalSpace = true; } }
}
```

The child generates `/Game/FX/NS_Blue`. It keeps `Speed`, overrides `Tint`, and inherits the
`Sparks` emitter with its additional `LocalSpace` setting. The emitter's `Emitters/E_Sparks`
reference still resolves relative to `Base.dfs`, even though the child lives in another directory.

This is source composition performed by DreamFX before generation. It does not create a native
Niagara System parent relationship. For native Niagara emitter relationships, use
[emitter asset inheritance](native-emitter-inheritance.md).

## Resolution and merge rules

`Parent` is allowed once in a `System` header, with a non-empty quoted source path. The `.dfs`
extension is optional. Resolution first checks the declaring file's directory, then the configured
DreamFX source roots, using the same rules as emitter `from` references. Parents can have parents;
cycles and chains longer than 128 source documents are rejected before generation changes an asset.

| Part | Child behavior |
| --- | --- |
| `Name`, `Root`, source identity | Always come from the child |
| `Settings` | Override matching keys; keep other parent settings |
| `Properties` | Override matching parameter names; append new parameters |
| Parameter type | Must match the inherited Niagara type; incompatible overrides are errors |
| System stacks | Replace a matching stack as a whole; retain other stacks |
| Emitters | Merge by emitter name; retain parent emitters and append new ones |
| Emitter settings and `Defaults` | Merge by setting or parameter name using the emitter override rules |
| Emitter stacks | Replace matching stack kinds; named `Stage` blocks match by stage name |
| Renderer declarations | A non-empty child renderer list replaces that emitter's renderer list |
| Emitter `from` | Keep the parent's reference and original source directory unless the child supplies another |

An explicit `from` or `inherits` on a child emitter selects its new base and clears the other base
kind. Other inherited overrides remain: switching the base does not discard the parent's settings,
defaults, or stacks already merged into that emitter.

Parameter declarations replace the whole inherited declaration, including their default and
attributes. An empty stack explicitly replaces the parent stack with an empty one. There is no
syntax in this version for deleting an inherited emitter, parameter, setting, or renderer list.
Names use Niagara's case-insensitive name identity. Duplicate parameter or emitter declarations
within one source are rejected instead of silently becoming overrides of each other.

The child's `Root` is also the default asset root during generation. When sharing a parent across
different content roots, write asset references with explicit `/Game/...`, `/PluginName/...`, or
`Plugin.Name:...` paths. File references such as `Parent` and emitter `from` retain the directory of
the source where they were declared.

## Rebuilds and verification

The generated child's source fingerprint includes the contents of every parent source and its
resolved `.dfe` dependencies. Editing only a parent makes ordinary `build` regenerate the child and
makes `verify` report drift. Verification does not apply the edit or save the asset. Fingerprints
use authored references and source content, so relocating the same source tree does not create
drift merely because its absolute checkout path changed.

The source watcher follows the same parent graph: edits to a parent `.dfs` or a referenced `.dfe`
queue affected children and grandchildren. The existing bulk rebuild threshold still applies.
Diagnostics retain the source file of inherited declarations, so an error in a parent points to
that parent file.

| Diagnostic | Meaning |
| --- | --- |
| `DFX2019` | Invalid `Parent` header argument |
| `DFX3050` | Parent source could not be found |
| `DFX3051` | Parent source could not be parsed or is not a System |
| `DFX3052` | Cyclic inheritance or excessive parent depth |
| `DFX3053` | Child parameter changes the inherited type |
| `DFX3054` | Duplicate parameter or emitter in one source layer |

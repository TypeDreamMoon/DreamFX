// The plan document's section 3.3 module, generated for real since plan-v2 W1.
//
// Tier one (plan 3.3): the whole Body becomes one UNiagaraNodeCustomHlsl. Inside the node editor the
// module is a black box -- which is the point of a text-first workflow, where the text is the source.
// What tier one buys over an inline `hlsl { }` at the point of use is everything a stack input cannot
// hold: multiple statements, named inputs with defaults and tooltips, and reuse by name.
//
// Namespaces work as they do in a .dfs. A bare name is one of this module's own inputs; anything
// with a namespace (`Particles.`, `Engine.`, `User.`, `System.`, `Emitter.`) is read straight off the
// parameter map, and a `Particles.` attribute can be written as well as read.
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
{
    Settings = {
        Usage       = ParticleUpdate;
        Category    = "MoonToon|Motion";
        Description = "Spins a sprite at a constant rate, optionally reversed.";
    }

    Inputs = {
        float SpinRate   = 90.0  [ Description="Degrees per second." ];
        // Tier one has no branch for a switch to select, so this is written as an ordinary input and
        // the build says so (DFX5102). The body reads it the same way.
        bool  bClockwise = true  [ StaticSwitch ];
        float RateScale  = 1.0   [ Advanced ];
    }

    Body = {
        float Dir = bClockwise ? 1.0 : -1.0;
        Particles.SpriteRotation += SpinRate * RateScale * Dir * Engine.DeltaTime;
    }
}

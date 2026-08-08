// Usage decides which stacks a module may be placed in; without it the module exists but is
// unreachable from every stack.
// EXPECT DFX3030
Module(Name="Corpus/M_MissingUsage", Root="Plugin.DreamFX")
{
    Settings = {
        Category = "Corpus";
    }

    Inputs = {
        float Rate = 1.0;
    }

    Body = {
        Particles.SpriteRotation += Rate * Engine.DeltaTime;
    }
}

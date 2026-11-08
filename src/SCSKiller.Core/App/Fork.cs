namespace SCSKiller.Core.App;

/// <summary>This build is an unofficial fork of SCSKiller (see README). A fork can't use SCSKiller's servers or its
/// signed update feeds, so both are switched off here: requests to the API routes fail as unreachable before anything
/// is sent (<see cref="RouteFailover"/>) and the updater treats every copy as not installed (<see cref="Updater"/>).
/// SCSKILLER_API (a backend of your own, as for development) turns the services back on.</summary>
public static class Fork
{
    public const string Name = "ShaderCompiler";
    public const string Subtitle = "Unofficial fork of SCSKiller";
    public const string Repo = "edmondnemsingh/ShaderCompiler";

    public const string ServicesNote =
        "SCSKiller's online services (community database, sign-in, sharing, beta updates) aren't available in this unofficial fork.";

    /// <summary>Whether the API routes may be used: only with SCSKILLER_API set.</summary>
    public static bool ServicesEnabled => Environment.GetEnvironmentVariable("SCSKILLER_API") is { Length: > 0 };
}

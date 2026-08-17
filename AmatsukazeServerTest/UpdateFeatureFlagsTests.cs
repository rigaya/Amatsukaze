using Amatsukaze.Server.Update;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class UpdateFeatureFlagsTests
{
    [Fact]
    public void ゲート無効時は本体更新対象を一覧から除外する()
    {
        var targets = UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
            selfUpdateEnabled: false);

        Assert.DoesNotContain(targets, target => target.IsApplication);
    }

    [Fact]
    public void ゲート有効時は本体更新対象を一覧に残す()
    {
        var application = Assert.Single(UpdateCatalog.Targets, target => target.IsApplication);

        var targets = UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
            selfUpdateEnabled: true);

        Assert.Contains(application, targets);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void 本体以外の対象はゲートの状態に影響されない(bool selfUpdateEnabled)
    {
        var encoder = Assert.Single(UpdateCatalog.Targets, target => target.Id == "x264");

        var targets = UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
            selfUpdateEnabled);

        Assert.Contains(encoder, targets);
    }

    [Fact]
    public void ゲート無効時は本体更新対象をIDで取得できない()
    {
        var application = Assert.Single(UpdateCatalog.Targets, target => target.IsApplication);

        var target = UpdateFeatureFlags.FindTarget(UpdateCatalog.Targets, application.Id,
            selfUpdateEnabled: false);

        Assert.Null(target);
    }
}

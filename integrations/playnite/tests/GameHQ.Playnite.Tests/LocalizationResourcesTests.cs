using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using GameHQ.Playnite.Localization;
using Xunit;

namespace GameHQ.Playnite.Tests;

public sealed class LocalizationResourcesTests
{
    private const string Prefix = "LOCGameHQIntegration";
    private static readonly XNamespace XamlNamespace =
        "http://schemas.microsoft.com/winfx/2006/xaml";
    private static string Output => AppContext.BaseDirectory;
    private static string LocalizationDirectory => Path.Combine(Output, "Localization");

    [Fact]
    public void EnabledGameHQLocalesMapExactlyToPlayniteResources()
    {
        using var registry = JsonDocument.Parse(File.ReadAllText(
            Path.Combine(Output, "Registry", "gamehq-locales.json")));
        using var map = JsonDocument.Parse(File.ReadAllText(
            Path.Combine(LocalizationDirectory, "locale-map.json")));

        var enabled = registry.RootElement.GetProperty("locales").EnumerateArray()
            .Where(locale => locale.GetProperty("tier").GetInt32() == 1 &&
                             locale.GetProperty("state").GetString() == "enabled")
            .ToDictionary(locale => locale.GetProperty("tag").GetString()!,
                          locale => locale.GetProperty("aliases").EnumerateArray()
                              .Select(alias => alias.GetString()).ToArray());
        var mapped = map.RootElement.GetProperty("locales").EnumerateArray()
            .ToDictionary(locale => locale.GetProperty("gamehq").GetString()!, locale => locale);

        Assert.Equal(enabled.Keys.OrderBy(value => value), mapped.Keys.OrderBy(value => value));
        var expectedNames = new Dictionary<string, string>
        {
            ["en-US"] = "en_US.xaml",
            ["zh-Hans"] = "zh_CN.xaml",
            ["ru-RU"] = "ru_RU.xaml",
            ["es-ES"] = "es_ES.xaml",
            ["pt-BR"] = "pt_BR.xaml",
            ["de-DE"] = "de_DE.xaml",
            ["ja-JP"] = "ja_JP.xaml",
            ["fr-FR"] = "fr_FR.xaml",
            ["pl-PL"] = "pl_PL.xaml",
            ["ko-KR"] = "ko_KR.xaml",
            ["zh-Hant"] = "zh_TW.xaml",
            ["tr-TR"] = "tr_TR.xaml"
        };

        foreach (var (tag, mapping) in mapped)
        {
            Assert.Equal(expectedNames[tag], mapping.GetProperty("resource").GetString());
            Assert.Equal(
                mapping.GetProperty("playnite").GetString() + ".xaml",
                mapping.GetProperty("resource").GetString());
            Assert.True(File.Exists(Path.Combine(LocalizationDirectory, expectedNames[tag])));
            Assert.Equal(
                enabled[tag].OrderBy(value => value),
                mapping.GetProperty("aliases").EnumerateArray()
                    .Select(alias => alias.GetString()).OrderBy(value => value));
        }

        var deferred = map.RootElement.GetProperty("deferred_tier_2").EnumerateArray()
            .Select(tag => tag.GetString()).OrderBy(value => value).ToArray();
        Assert.Equal(new[] { "es-419", "it-IT", "th-TH", "uk-UA" }, deferred);
        Assert.DoesNotContain(mapped.Keys, tag => deferred.Contains(tag));
        Assert.Equal(
            expectedNames.Values.OrderBy(value => value),
            Directory.EnumerateFiles(LocalizationDirectory, "*.xaml")
                .Select(Path.GetFileName).OrderBy(value => value));
    }

    [Fact]
    public void EveryTierOneResourceIsCompleteAndStructurallySafe()
    {
        var english = ReadDictionary("en_US.xaml");
        Assert.Equal(52, english.Count);
        Assert.All(english.Keys, key => Assert.StartsWith(Prefix, key));

        foreach (var path in Directory.EnumerateFiles(LocalizationDirectory, "*.xaml"))
        {
            var localized = ReadDictionary(Path.GetFileName(path));
            Assert.Equal(english.Keys.OrderBy(key => key), localized.Keys.OrderBy(key => key));

            foreach (var (key, source) in english)
            {
                var target = localized[key];
                Assert.False(string.IsNullOrWhiteSpace(target));
                Assert.Equal(Placeholders(source), Placeholders(target));
                foreach (var token in new[] { "GameHQ", "Playnite", "GameHQ.exe", "app\\GameHQ.exe" })
                {
                    if (source.Contains(token, StringComparison.Ordinal))
                        Assert.Contains(token, target, StringComparison.Ordinal);
                }
            }

            if (!path.EndsWith("en_US.xaml", StringComparison.OrdinalIgnoreCase))
                Assert.True(localized.Count(entry => entry.Value != english[entry.Key]) >= 40);
        }
    }

    [Fact]
    public void CSharpLookupUsesHostLocaleAndFallsBackToPackagedEnglish()
    {
        var englishPath = Path.Combine(LocalizationDirectory, "en_US.xaml");
        var localized = new PluginLocalization(
            key => key == Prefix + "OpenFolder" ? "LOCALIZED" : key,
            englishPath);
        Assert.Equal("LOCALIZED", localized.Get(Prefix + "OpenFolder"));
        Assert.Equal("Open GameHQ", localized.Get(Prefix + "OpenGameHQ"));

        var unavailable = new PluginLocalization(_ => throw new InvalidOperationException(), englishPath);
        Assert.Equal("Test failed: offline", unavailable.Format(Prefix + "TestFailedFormat", "offline"));
        Assert.Equal("GameHQ localization unavailable", unavailable.Get(Prefix + "MissingKey"));
    }

    [Fact]
    public void UserFacingXamlAndCSharpHaveNoRemainingEnglishLiterals()
    {
        var english = ReadDictionary("en_US.xaml");
        var view = XDocument.Load(Path.Combine(Output, "Audit", "GameHQIntegrationSettingsView.xaml"));
        foreach (var attribute in view.Descendants().Attributes()
                     .Where(attribute => attribute.Name.LocalName is "Text" or "Content"))
        {
            if (attribute.Value.StartsWith("{Binding", StringComparison.Ordinal))
                continue;
            Assert.StartsWith("{DynamicResource " + Prefix, attribute.Value);
            var key = attribute.Value.TrimStart('{').TrimEnd('}').Split(' ')[1];
            Assert.Contains(key, english.Keys);
        }

        var source = StripComments(
            File.ReadAllText(Path.Combine(Output, "Audit", "GameHQIntegrationSettingsView.xaml")) +
            File.ReadAllText(Path.Combine(Output, "Audit", "GameHQIntegrationSettingsViewModel.cs")) +
            File.ReadAllText(Path.Combine(Output, "Audit", "GameHQPlugin.cs")));
        foreach (var oldLiteral in new[]
        {
            "\"Connected\"", "\"Connecting...\"", "\"Disconnected\"",
            "\"Open GameHQ\"", "\"Testing...\"", "\"Connection test succeeded.\"",
            "\"The selected path is not a valid GameHQ install.\"",
            "\"GameHQ Integration\""
        })
            Assert.DoesNotContain(oldLiteral, source, StringComparison.Ordinal);

        Assert.Contains("ResourceProvider.GetString", source, StringComparison.Ordinal);
        Assert.Contains("_strings.Get(\"" + Prefix, source, StringComparison.Ordinal);
        Assert.Equal(
            english.Keys.OrderBy(key => key),
            Regex.Matches(source, Prefix + @"[A-Za-z0-9]+")
                .Select(match => match.Value).Distinct().OrderBy(key => key));

        var manifest = File.ReadAllText(Path.Combine(Output, "Audit", "extension.yaml"));
        Assert.Contains("Name: GameHQ Integration", manifest, StringComparison.Ordinal);
        Assert.DoesNotContain("Name: LOC", manifest, StringComparison.Ordinal);
    }

    [Fact]
    public void BuildAndPackageRulesIncludeLooseLocalizationResources()
    {
        var project = File.ReadAllText(Path.Combine(Output, "Audit", "GameHQ.Playnite.csproj"));
        Assert.Contains("<Page Remove=\"Localization\\*.xaml\"", project, StringComparison.Ordinal);
        Assert.Contains("<Content Include=\"Localization\\*.xaml\"", project, StringComparison.Ordinal);
        Assert.Contains("Localization\\locale-map.json", project, StringComparison.Ordinal);

        var package = File.ReadAllText(Path.Combine(Output, "Audit", "package.ps1"));
        Assert.Contains("Copy-Item -LiteralPath $localizationSource", package, StringComparison.Ordinal);
        Assert.Contains("$expectedLocalizationEntries", package, StringComparison.Ordinal);
    }

    private static Dictionary<string, string> ReadDictionary(string fileName)
    {
        var document = XDocument.Load(Path.Combine(LocalizationDirectory, fileName));
        return document.Root!.Elements()
            .Where(element => element.Name.LocalName == "String")
            .ToDictionary(
                element => (string)element.Attribute(XamlNamespace + "Key")!,
                element => element.Value,
                StringComparer.Ordinal);
    }

    private static string[] Placeholders(string value) =>
        Regex.Matches(value, @"\{\d+\}").Select(match => match.Value).OrderBy(item => item).ToArray();

    private static string StripComments(string source) =>
        Regex.Replace(source, @"//.*?$|/\*.*?\*/", string.Empty,
                      RegexOptions.Multiline | RegexOptions.Singleline);
}

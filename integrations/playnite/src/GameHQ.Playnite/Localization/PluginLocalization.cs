using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Xml.Linq;

namespace GameHQ.Playnite.Localization
{
    // Playnite owns locale selection and its ResourceProvider owns the active
    // dictionary. This wrapper adds a package-local en_US fallback for missing
    // resources/keys without introducing a second language preference.
    internal sealed class PluginLocalization
    {
        private const string MissingFallback = "GameHQ localization unavailable";
        private static readonly XNamespace XamlNamespace =
            "http://schemas.microsoft.com/winfx/2006/xaml";

        private readonly Func<string, string> _lookup;
        private readonly IReadOnlyDictionary<string, string> _english;

        internal PluginLocalization(Func<string, string> lookup, string englishResourcePath)
        {
            _lookup = lookup ?? throw new ArgumentNullException(nameof(lookup));
            _english = LoadDictionary(englishResourcePath);
        }

        internal string Get(string key)
        {
            if (string.IsNullOrWhiteSpace(key))
                return MissingFallback;

            try
            {
                var localized = _lookup(key);
                if (!string.IsNullOrWhiteSpace(localized) &&
                    !string.Equals(localized, key, StringComparison.Ordinal))
                    return localized;
            }
            catch (Exception)
            {
                // A missing or malformed active-locale resource must not take
                // down the settings page. The complete en_US file is local to
                // this plugin package and is loaded below.
            }

            string fallback;
            return _english.TryGetValue(key, out fallback) ? fallback : MissingFallback;
        }

        internal string Format(string key, params object[] arguments)
        {
            return string.Format(CultureInfo.CurrentUICulture, Get(key), arguments);
        }

        private static IReadOnlyDictionary<string, string> LoadDictionary(string path)
        {
            try
            {
                var document = XDocument.Load(path, LoadOptions.PreserveWhitespace);
                return document.Root == null
                    ? new Dictionary<string, string>()
                    : document.Root.Elements()
                        .Where(element => element.Name.LocalName == "String")
                        .Select(element => new
                        {
                            Key = (string)element.Attribute(XamlNamespace + "Key"),
                            Value = element.Value
                        })
                        .Where(entry => !string.IsNullOrWhiteSpace(entry.Key))
                        .ToDictionary(entry => entry.Key, entry => entry.Value,
                                      StringComparer.Ordinal);
            }
            catch (Exception)
            {
                return new Dictionary<string, string>();
            }
        }
    }
}

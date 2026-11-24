using System;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GameHQ.Playnite.Protocol
{
    // The net462 tests link the production JavaScriptSerializer implementation.
    // This field bag lets the same lifecycle tests run on the portable net8 host.
    internal sealed class IntegrationMessage
    {
        private readonly JsonObject _fields;

        public IntegrationMessage(string type) : this(new JsonObject { ["type"] = type })
        {
            if (string.IsNullOrEmpty(type)) throw new ArgumentException("type is required", nameof(type));
        }

        private IntegrationMessage(JsonObject fields) => _fields = fields;

        public string Type => GetString("type");

        public IntegrationMessage Set(string key, object value)
        {
            _fields[key] = JsonSerializer.SerializeToNode(value);
            return this;
        }

        public string GetString(string key) =>
            _fields[key] is JsonValue node && node.TryGetValue(out string value) ? value : null;

        public int? GetInt(string key) =>
            int.TryParse(_fields[key]?.ToString(), out int value) ? value : null;

        public byte[] ToUtf8Json() => JsonSerializer.SerializeToUtf8Bytes(_fields);

        public static IntegrationMessage FromUtf8Json(byte[] payload)
        {
            if (JsonNode.Parse(payload) is not JsonObject fields || !fields.ContainsKey("type"))
                throw new FormatException("Message is missing a type field");
            return new IntegrationMessage(fields);
        }
    }
}

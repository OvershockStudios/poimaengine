// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
namespace Poima.Editor;

public sealed class FakeHost
{
    public event EventHandler? StateChanged;
    public JsonObject State { get; } = JsonNode.Parse("""{"runtime":{"structure_revision":4},"components":{"revision":9}}""")!.AsObject();
    public JsonObject Observation { get; set; } = new();
    public JsonObject? Sent { get; private set; }
    public int Writes { get; private set; }
    public JsonObject Call(string method, JsonObject args)
    {
        if (method == "runtime.component.get") return Observation.DeepClone().AsObject();
        if (method != "runtime.component.edit") throw new InvalidOperationException(method);
        Sent=args.DeepClone().AsObject(); ++Writes;
        Observation["values"]=args["values"]!.DeepClone();
        return new();
    }
    public void RefreshState() => StateChanged?.Invoke(this, EventArgs.Empty);
}
public sealed class FakeGameplay { public void RequireClean() {} }
public sealed class EditorModel
{
    public FakeHost Host { get; } = new();
    public FakeGameplay? Gameplay => null;
    public bool Paused => true;
    public string RuntimeId => "session";
    public long Tick => 3;
    public static string NewId() => Guid.NewGuid().ToString("N");
    public void RequireAuthoredClean() {}
    public void Note(string text) {}
}
internal static class Program
{
    static int checks;
    static void Check(bool ok, string message) { ++checks; if (!ok) throw new Exception(message); }
    static JsonObject Field(string kind, int capacity=3) => new() { ["name"]="Items", ["kind"]="array", ["element_kind"]=kind, ["capacity"]=capacity };
    static JsonArray Read(string kind, string text, int capacity=3) => ComponentFields.Parse(Field(kind,capacity),text).AsArray();
    static void Bad(string kind, string text, int capacity=3)
    {
        try { _=Read(kind,text,capacity); } catch (InvalidOperationException) { ++checks; return; }
        throw new Exception($"Accepted invalid {kind} array: {text}");
    }
    static int Main()
    {
        Check(Read("int32","[-2147483648,0,2147483647]").ToJsonString()=="[-2147483648,0,2147483647]", "Integer bounds failed.");
        Check(Read("int64","[\"-9223372036854775808\",\"0\",\"9223372036854775807\"]").Count==3,"Wide bounds failed.");
        Check(Read("float32","[-0,-1e-50,1.5]")[0]!.GetValue<float>()==0,"Single zero failed.");
        Check(BitConverter.SingleToInt32Bits(Read("float32","[-1e-50]")[0]!.GetValue<float>())==0,"Single underflow retained negative zero.");
        Check(BitConverter.DoubleToInt64Bits(Read("float64","[-0.0]")[0]!.GetValue<double>())==0,"Double retained negative zero.");
        Check(Read("entity","[\"00000000000000000000000000000000\",\"abcdef0123456789abcdef0123456789\"]").Count==2,"Entity parsing failed.");
        Check(Read("int32","[]").Count==0,"Empty buffer rejected.");
        Check(Read("int32","["+string.Join(',',Enumerable.Repeat("1",31))+"]",31).Count==31,"Capacity boundary failed.");
        foreach(var kind in new[]{"int32","int64","float32","float64","entity"})
            foreach(var text in new[]{"null","{}","[null]","[true]","[[1]]","[{}]","[1,]","[1/*comment*/]","["}) Bad(kind,text);
        foreach(var text in new[]{"[\"1\"]","[1.0]","[1e0]","[2147483648]","[-2147483649]","[1,2,3,4]"}) Bad("int32",text);
        foreach(var text in new[]{"[1]","[\"+1\"]","[\"01\"]","[\"-0\"]","[\" 1\"]","[\"9223372036854775808\"]","[\"1.0\"]"}) Bad("int64",text);
        foreach(var kind in new[]{"float32","float64"}) foreach(var text in new[]{"[\"1\"]","[\"NaN\"]","[1e999]"}) Bad(kind,text);
        Bad("float32","[3.5e38]"); Bad("entity","[\"ABCDEF0123456789abcdef0123456789\"]"); Bad("entity","[0]");
        Bad("array","[]");Bad("int32","[]",0);Bad("int32","[]",32);Bad("int32","["+new string(' ',16384)+"]");
        // Exercise the linked production Apply method: changing a scalar must
        // preserve an untouched typed array and the originally observed guards.
        var editor=new EditorModel(); var array=Field("int64");array["id"]="items";
        editor.Host.Observation=new() { ["session_id"]="session",["tick"]=3L,["structure_revision"]=4L,["component_revision"]=9L,
            ["id"]="entity",["type"]="inventory",["schema"]=new JsonObject { ["fields"]=new JsonArray(array,new JsonObject { ["id"]="count",["name"]="Count",["kind"]="int32" }) },
            ["values"]=new JsonObject { ["items"]=JsonNode.Parse("[\"9223372036854775807\"]"),["count"]=1 } };
        using var model=new ComponentEditorModel(editor);model.Load("entity","inventory");model.Set("count","2");model.Apply();
        Check(editor.Host.Writes==1 && editor.Host.Sent!["values"]!["items"]!.ToJsonString()=="[\"9223372036854775807\"]","Scalar edit lost untouched collection.");
        Check(editor.Host.Sent!["expected_revision"]!.GetValue<long>()==9 && editor.Host.Sent["expected_structure_revision"]!.GetValue<long>()==4,"Apply replaced observed guards.");
        model.Set("items","[1]");Check(model.Invalid(array),"Invalid draft accepted.");
        try { model.Apply(); throw new Exception("Invalid array applied."); } catch(InvalidOperationException) {}
        Check(editor.Host.Writes==1,"Invalid draft reached native host.");
        Console.WriteLine($"PASS ComponentFields and linked ComponentEditorModel: {checks} checks; no GUI/native-device qualification.");return 0;
    }
}

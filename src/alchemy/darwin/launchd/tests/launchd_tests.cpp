// Unit tests of the session launchd's pieces that need no process: the
// property-list reader and writer, and the job model (ctest alchemy_launchd_units).

#include "alchemy/darwin/launchd/job.hpp"
#include "alchemy/darwin/launchd/plist.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace alchemy::launchd;

namespace {

int failures = 0;

void require(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}

bool throws(const std::function<void()>& body) {
    try { body(); } catch (const std::exception&) { return true; }
    return false;
}

Value xml(std::string_view text) {
    return parse_plist(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void xml_reader() {
    const auto value = xml(
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><!-- a comment -->\n<dict>\n"
        "  <key>Label</key> <string>com.example.a&amp;b &lt;x&gt; &#65;&#x42;</string>\n"
        "  <key>Count</key><integer> -42 </integer>\n"
        "  <key>Big</key><integer>0x10</integer>\n"
        "  <key>Ratio</key><real>2.5</real>\n"
        "  <key>Yes</key><true/>\n  <key>No</key><false/>\n"
        "  <key>When</key><date>2024-02-29T12:34:56Z</date>\n"
        "  <key>Blob</key><data>aGVs\n bG8=</data>\n"
        "  <key>Empty</key><string/>\n  <key>None</key><array/>\n  <key>Nothing</key><dict/>\n"
        "  <key>List</key><array><string>a</string><string><![CDATA[<raw>]]></string></array>\n"
        "</dict></plist>\n");
    require(value.is(Value::Kind::dictionary) && value.dictionary.size() == 12, "xml: a dictionary of 12 members in order");
    require(value.find("Label") && value.find("Label")->text == "com.example.a&b <x> AB", "xml: entities and character references");
    require(value.find("Count")->integer == -42 && value.find("Big")->integer == 16, "xml: integers (decimal, hex, trimmed)");
    require(value.find("Ratio")->real == 2.5, "xml: real");
    require(value.find("Yes")->boolean && !value.find("No")->boolean, "xml: booleans");
    require(value.find("When")->is(Value::Kind::date) && value.find("When")->text == "2024-02-29T12:34:56Z", "xml: date");
    const auto& blob = value.find("Blob")->data;
    require(std::string(blob.begin(), blob.end()) == "hello", "xml: base64 data across whitespace");
    require(value.find("Empty")->text.empty() && value.find("None")->array.empty() && value.find("Nothing")->dictionary.empty(), "xml: empty elements");
    require(value.find("List")->array.size() == 2 && value.find("List")->array[1].text == "<raw>", "xml: array with CDATA");
    require(value.dictionary.front().first == "Label", "xml: file order kept");

    require(throws([] { xml("<plist><dict><key>a</key></dict></plist>"); }), "xml: a key without a value is refused");
    require(throws([] { xml("<plist><dict><key>a</key><string>1</string><key>a</key><string>2</string></dict></plist>"); }), "xml: duplicate keys are refused");
    require(throws([] { xml("<plist><integer>12x</integer></plist>"); }), "xml: bad integer");
    require(throws([] { xml("<plist><date>yesterday</date></plist>"); }), "xml: bad date");
    require(throws([] { xml("<notplist/>"); }), "xml: wrong root element");
    require(throws([] { xml("<plist><string>&bogus;</string></plist>"); }), "xml: unknown entity");
    require(throws([] { xml("<plist><dict>"); }), "xml: truncated");
}

// A small binary plist writer for the reader's test (bplist00, 1-byte offsets and refs).
std::vector<std::uint8_t> binary_sample() {
    std::vector<std::vector<std::uint8_t>> objects;
    const auto ascii = [](std::string_view text) {
        std::vector<std::uint8_t> out{static_cast<std::uint8_t>(0x50 | text.size())};
        out.insert(out.end(), text.begin(), text.end());
        return out;
    };
    // 0: dict {Label: 1, N: 3, B: true(5), A: [6], U: 8, D: date(9)}
    objects.push_back({0xd6, 1, 2, 4, 10, 11, 12, /* values */ 13, 3, 5, 6, 8, 9});
    objects.push_back(ascii("ignored"));   // 1 (replaced below)
    objects.push_back(ascii("N"));         // 2
    objects.push_back({0x11, 0x01, 0x2c});  // 3: integer 300
    objects.push_back(ascii("B"));         // 4
    objects.push_back({0x09});             // 5: true
    objects.push_back({0xa1, 7});          // 6: array [7]
    objects.push_back(ascii("item"));      // 7
    // 8: UTF-16 "é"
    objects.push_back({0x61, 0x00, 0xe9});
    // 9: date 0.0 (2001-01-01T00:00:00Z)
    objects.push_back({0x33, 0, 0, 0, 0, 0, 0, 0, 0});
    objects.push_back(ascii("A"));         // 10
    objects.push_back(ascii("U"));         // 11
    objects.push_back(ascii("D"));         // 12
    objects.push_back(ascii("com.example.bp"));   // 13: Label's value (under 15 bytes: one marker)
    objects[1] = ascii("Label");
    std::vector<std::uint8_t> out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<std::uint8_t> offsets;
    for (const auto& object : objects) {
        offsets.push_back(static_cast<std::uint8_t>(out.size()));
        out.insert(out.end(), object.begin(), object.end());
    }
    const auto table = out.size();
    out.insert(out.end(), offsets.begin(), offsets.end());
    std::uint8_t trailer[32]{};
    trailer[6] = 1; trailer[7] = 1;
    trailer[15] = static_cast<std::uint8_t>(objects.size());
    trailer[23] = 0;
    trailer[31] = static_cast<std::uint8_t>(table);
    out.insert(out.end(), trailer, trailer + 32);
    return out;
}

void binary_reader() {
    const auto bytes = binary_sample();
    const auto value = parse_plist(bytes);
    require(value.is(Value::Kind::dictionary) && value.dictionary.size() == 6, "binary: a dictionary of 6");
    require(value.find("Label") && value.find("Label")->text == "com.example.bp", "binary: ASCII string");
    require(value.find("N") && value.find("N")->integer == 300, "binary: integer");
    require(value.find("B") && value.find("B")->boolean, "binary: true");
    require(value.find("A") && value.find("A")->array.size() == 1 && value.find("A")->array[0].text == "item", "binary: array");
    require(value.find("U") && value.find("U")->text == "\xC3\xA9", "binary: UTF-16 string to UTF-8");
    require(value.find("D") && value.find("D")->text == "2001-01-01T00:00:00Z", "binary: date");
    auto broken = bytes;
    broken[broken.size() - 1] = 0xff;   // offset table outside the file
    require(throws([&] { static_cast<void>(parse_plist(broken)); }), "binary: a bad trailer is refused");
}

void xml_writer() {
    auto value = Value::of_dictionary();
    value.set("Label", Value::of_string("a<b>&c"));
    value.set("N", Value::of_integer(-7));
    value.set("On", Value::of_boolean(true));
    value.set("List", Value::of_array({Value::of_string("x")}));
    Value data; data.kind = Value::Kind::data; data.data = {'h', 'i', '!'};
    value.set("Data", data);
    value.set("Empty", Value::of_dictionary());
    const auto text = to_xml(value);
    const auto again = xml(text);
    require(again.find("Label") && again.find("Label")->text == "a<b>&c", "writer: escaped string round trip");
    require(again.find("N")->integer == -7 && again.find("On")->boolean, "writer: integer and boolean round trip");
    require(again.find("List")->array.size() == 1 && again.find("Data")->data.size() == 3, "writer: array and data round trip");
    require(again.find("Empty")->is(Value::Kind::dictionary), "writer: empty dictionary");
    value.set("N", Value::of_integer(9));
    require(value.dictionary.size() == 6 && value.find("N")->integer == 9, "set replaces in place");
    value.erase("N");
    require(!value.find("N"), "erase");
}

Value job(std::string_view members) {
    return xml("<plist><dict>" + std::string(members) + "</dict></plist>");
}

void job_model() {
    const auto spec = parse_job(job(
        "<key>Label</key><string>com.example.job</string>"
        "<key>ProgramArguments</key><array><string>/usr/bin/tool</string><string>--flag</string></array>"
        "<key>RunAtLoad</key><true/>"
        "<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/><key>PathState</key><dict><key>/tmp/x</key><true/></dict>"
        "<key>NetworkState</key><true/></dict>"
        "<key>StartInterval</key><integer>3600</integer>"
        "<key>StartCalendarInterval</key><array><dict><key>Hour</key><integer>3</integer><key>Minute</key><integer>15</integer></dict></array>"
        "<key>EnvironmentVariables</key><dict><key>A</key><string>1</string></dict>"
        "<key>StandardOutPath</key><string>/tmp/out.log</string>"
        "<key>MachServices</key><dict><key>com.example.job.xpc</key><true/><key>com.example.off</key><false/>"
        "<key>com.example.hidden</key><dict><key>HideUntilCheckIn</key><true/></dict></dict>"
        "<key>Sockets</key><dict><key>Listeners</key><dict><key>SockServiceName</key><string>8080</string></dict></dict>"
        "<key>ProcessType</key><string>Background</string>"
        "<key>SomethingNew</key><string>x</string>"));
    require(spec.label == "com.example.job" && spec.program == "/usr/bin/tool" && spec.arguments.size() == 2, "job: label, program from ProgramArguments");
    require(spec.run_at_load && spec.keep_alive.successful_exit == false && spec.keep_alive.path_state.at("/tmp/x"), "job: RunAtLoad and KeepAlive conditions");
    require(spec.start_interval == 3600u && spec.calendar.size() == 1 && spec.calendar[0].hour == 3 && spec.calendar[0].minute == 15 && !spec.calendar[0].day,
            "job: StartInterval and StartCalendarInterval");
    require(spec.environment.size() == 1 && spec.standard_out == "/tmp/out.log", "job: environment and stdout path");
    require(spec.mach_services.size() == 2 && spec.mach_services[1].hide_until_check_in, "job: MachServices (false entries dropped)");
    require(spec.sockets.size() == 1 && spec.sockets[0].service == "8080" && spec.sockets[0].stream && spec.sockets[0].passive, "job: Sockets");
    const auto ignored = [&](std::string_view key) { for (const auto& item : spec.ignored_keys) if (item == key) return true; return false; };
    require(ignored("ProcessType") && ignored("SomethingNew") && ignored("KeepAlive.NetworkState"), "job: keys without effect are listed");
    require(spec.throttle_interval == 10 && spec.exit_timeout == 20, "job: default throttle and exit timeout");

    const auto program = parse_job(job("<key>Label</key><string>p</string><key>Program</key><string>/bin/x</string><key>OnDemand</key><false/>"));
    require(program.arguments == std::vector<std::string>{"/bin/x"} && program.keep_alive.always, "job: Program alone, OnDemand false");
    const auto relative = parse_job(job("<key>Label</key><string>r</string><key>ProgramArguments</key><array><string>tool</string></array>"));
    require(relative.program == "tool", "job: a PATH-searched ProgramArguments[0]");

    require(throws([] { parse_job(job("<key>Program</key><string>/bin/x</string>")); }), "job: no Label");
    require(throws([] { parse_job(job("<key>Label</key><string>x</string>")); }), "job: no program");
    require(throws([] { parse_job(job("<key>Label</key><string>x</string><key>Program</key><string>relative</string>")); }), "job: relative Program");
    require(throws([] { parse_job(job("<key>Label</key><string>x</string><key>Program</key><string>/x</string><key>RunAtLoad</key><string>yes</string>")); }),
            "job: a key of the wrong type");
    require(throws([] { parse_job(job("<key>Label</key><string>x</string><key>Program</key><string>/x</string><key>KeepAlive</key><dict><key>Bogus</key><true/></dict>")); }),
            "job: an unknown KeepAlive condition");

    require(loads_in_session(job("<key>Label</key><string>x</string>")), "session: no limit");
    require(loads_in_session(job("<key>LimitLoadToSessionType</key><array><string>LoginWindow</string><string>Aqua</string></array>")), "session: Aqua in a list");
    require(!loads_in_session(job("<key>LimitLoadToSessionType</key><string>System</string>")), "session: System is refused");
}

} // namespace

int main() {
    xml_reader();
    binary_reader();
    xml_writer();
    job_model();
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "ok launchd units\n";
    return 0;
}

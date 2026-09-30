#include "ircli/command.h"
#include "ircli/internal/command_line.h"

#include "ircore/processing.h"
#include "ircore/version.h"

#include <charconv>
#include <format>
#include <map>
#include <ostream>
#include <system_error>

namespace ircli {

namespace {

constexpr std::string_view usage_text =
    R"(Usage:
  spectrallab [options] <input.dng> <output.dng>
  spectrallab [options] --output-dir <dir> <input.dng>...
  spectrallab --analyze [options] <input.dng>...

Converts infrared DNG files into editor-ready linear DNG files.

Options:
  --wb <mode>            White balance: auto (default), as-shot,
                         area=X,Y,W,H (reference area in pixels),
                         neutral=R,G,B (camera neutral)
  --filter <type>        auto (default), 590, 665, 720, 850, full-spectrum
  --mapping <mode>       auto (default), native, false-color, monochrome
  --compression <type>   lossless-jpeg (default), none
  --no-thumbnail         Do not embed a preview in the output
  -o, --output-dir <dir> Write outputs to <dir>, keeping the file names
  -f, --force            Overwrite existing output files
  --threads <n>          Worker threads (default: all cores)
  --analyze              Only report white balance and filter detection
  -q, --quiet            Print errors only
  -h, --help             Show this help
  --version              Show the version
)";

template <typename T> std::optional<T> parse_number(std::string_view text) {
  T value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

template <typename T>
std::optional<std::vector<T>> parse_list(std::string_view text,
                                         std::size_t count) {
  std::vector<T> values;
  for (;;) {
    const std::size_t comma = text.find(',');
    const auto value = parse_number<T>(text.substr(0, comma));
    if (!value) {
      return std::nullopt;
    }
    values.push_back(*value);
    if (comma == std::string_view::npos) {
      break;
    }
    text.remove_prefix(comma + 1);
  }
  if (values.size() != count) {
    return std::nullopt;
  }
  return values;
}

void print_report(std::ostream &out, const ircore::ProcessingReport &report) {
  out << std::format("  camera:        {}\n",
                     report.camera.empty() ? "unknown" : report.camera);
  out << std::format("  size:          {}x{} ({})\n", report.width,
                     report.height,
                     report.input_photometric == ircore::Photometric::cfa
                         ? "CFA"
                         : "LinearRaw");
  const auto &n = report.camera_neutral;
  out << std::format("  white balance: {} (neutral {:.4f},{:.4f},{:.4f})\n",
                     ircore::to_string(report.white_balance_mode), n[0], n[1],
                     n[2]);
  const auto &d = report.detection;
  if (report.filter_detected) {
    out << std::format("  filter:        {} (detected, {} confidence; "
                       "balance {:.3f}, chroma spread {:.3f})\n",
                       ircore::to_string(report.filter),
                       ircore::to_string(d.confidence), d.channel_balance,
                       d.chroma_spread);
  } else {
    out << std::format("  filter:        {} (set; detection would say {}, "
                       "balance {:.3f}, chroma spread {:.3f})\n",
                       ircore::to_string(report.filter),
                       ircore::to_string(d.filter), d.channel_balance,
                       d.chroma_spread);
  }
  out << std::format("  mapping:       {}\n", ircore::to_string(report.mapping));
  out << std::format("  camera matrix: illuminant {}{}\n",
                     report.reference_illuminant,
                     report.used_forward_matrix ? ", forward matrix" : "");
  out << std::format("  clipped:       {:.2f}%\n",
                     report.clipped_fraction * 100.0);
  for (const auto &warning : report.warnings) {
    out << "  warning: " << warning << '\n';
  }
}

} // namespace

namespace detail {

std::expected<ircore::WhiteBalanceSettings, std::string>
parse_white_balance(std::string_view text) {
  ircore::WhiteBalanceSettings settings;
  if (text == "auto") {
    settings.mode = ircore::WhiteBalanceMode::auto_ir;
  } else if (text == "as-shot") {
    settings.mode = ircore::WhiteBalanceMode::as_shot;
  } else if (text.starts_with("area=")) {
    const auto values = parse_list<std::uint32_t>(text.substr(5), 4);
    if (!values || (*values)[2] < 2 || (*values)[3] < 2) {
      return std::unexpected(
          "--wb area expects X,Y,W,H in pixels (W and H at least 2)");
    }
    settings.mode = ircore::WhiteBalanceMode::reference_area;
    settings.area = {(*values)[0], (*values)[1], (*values)[2], (*values)[3]};
  } else if (text.starts_with("neutral=")) {
    const auto values = parse_list<double>(text.substr(8), 3);
    if (!values || (*values)[0] <= 0.0 || (*values)[1] <= 0.0 ||
        (*values)[2] <= 0.0) {
      return std::unexpected("--wb neutral expects three positive numbers R,G,B");
    }
    settings.mode = ircore::WhiteBalanceMode::explicit_neutral;
    settings.neutral = {(*values)[0], (*values)[1], (*values)[2]};
  } else {
    return std::unexpected(std::format("unknown white balance mode: {}", text));
  }
  return settings;
}

std::expected<CommandLine, std::string>
parse_command_line(std::span<const std::string_view> arguments) {
  CommandLine result;
  bool options_ended = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    std::string_view argument = arguments[i];
    if (options_ended || !argument.starts_with('-') || argument == "-") {
      result.positional.emplace_back(argument);
      continue;
    }
    if (argument == "--") {
      options_ended = true;
      continue;
    }

    std::string_view name = argument;
    std::optional<std::string_view> inline_value;
    if (const std::size_t equals = argument.find('=');
        argument.starts_with("--") && equals != std::string_view::npos) {
      name = argument.substr(0, equals);
      inline_value = argument.substr(equals + 1);
    }
    const auto value = [&]() -> std::expected<std::string_view, std::string> {
      if (inline_value) {
        return *inline_value;
      }
      if (i + 1 >= arguments.size()) {
        return std::unexpected(std::format("{} needs a value", name));
      }
      return arguments[++i];
    };
    const auto flag = [&](bool &target) -> std::expected<void, std::string> {
      if (inline_value) {
        return std::unexpected(std::format("{} does not take a value", name));
      }
      target = true;
      return {};
    };

    std::expected<void, std::string> status;
    if (name == "-h" || name == "--help") {
      status = flag(result.help);
    } else if (name == "--version") {
      status = flag(result.version);
    } else if (name == "--analyze") {
      status = flag(result.analyze);
    } else if (name == "-f" || name == "--force") {
      status = flag(result.force);
    } else if (name == "-q" || name == "--quiet") {
      status = flag(result.quiet);
    } else if (name == "--no-thumbnail") {
      bool disabled = false;
      status = flag(disabled);
      result.options.embed_thumbnail = !disabled;
    } else if (name == "-o" || name == "--output-dir") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      result.output_dir = std::filesystem::path(*text);
    } else if (name == "--wb") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      const auto settings = parse_white_balance(*text);
      if (!settings) {
        return std::unexpected(settings.error());
      }
      result.options.white_balance = *settings;
    } else if (name == "--filter") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      const auto filter = ircore::parse_filter_type(*text);
      if (!filter) {
        return std::unexpected(std::format("unknown filter type: {}", *text));
      }
      result.options.filter = *filter;
    } else if (name == "--mapping") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      const auto mapping = ircore::parse_channel_mapping(*text);
      if (!mapping) {
        return std::unexpected(std::format("unknown channel mapping: {}", *text));
      }
      result.options.mapping = *mapping;
    } else if (name == "--compression") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      const auto compression = ircore::parse_compression(*text);
      if (!compression) {
        return std::unexpected(std::format("unknown compression: {}", *text));
      }
      result.options.compression = *compression;
    } else if (name == "--threads") {
      const auto text = value();
      if (!text) {
        return std::unexpected(text.error());
      }
      const auto threads = parse_number<unsigned>(*text);
      if (!threads || *threads == 0 || *threads > 1024) {
        return std::unexpected("--threads expects a number from 1 to 1024");
      }
      result.options.threads = *threads;
    } else {
      return std::unexpected(std::format("unknown option: {}", argument));
    }
    if (!status) {
      return std::unexpected(status.error());
    }
  }
  return result;
}

std::expected<std::vector<Job>, std::string>
plan_jobs(const CommandLine &command_line) {
  const auto &positional = command_line.positional;
  if (positional.empty()) {
    return std::unexpected("no input files");
  }
  std::vector<Job> jobs;
  if (command_line.analyze) {
    if (command_line.output_dir) {
      return std::unexpected("--analyze does not write files; drop "
                             "--output-dir");
    }
    for (const auto &input : positional) {
      jobs.push_back({input, {}});
    }
    return jobs;
  }

  std::optional<std::filesystem::path> directory = command_line.output_dir;
  std::vector<std::filesystem::path> inputs = positional;
  if (!directory) {
    std::error_code error;
    if (positional.size() == 2 &&
        !std::filesystem::is_directory(positional[1], error)) {
      jobs.push_back({positional[0], positional[1]});
    } else if (positional.size() >= 2 &&
               std::filesystem::is_directory(positional.back(), error)) {
      directory = positional.back();
      inputs.pop_back();
    } else {
      return std::unexpected(
          "expected <input.dng> <output.dng>, or use --output-dir for several "
          "inputs");
    }
  }
  if (directory) {
    std::map<std::filesystem::path, std::filesystem::path> seen;
    for (const auto &input : inputs) {
      auto output = *directory / input.filename();
      output.replace_extension(".dng");
      if (const auto [it, inserted] = seen.emplace(output, input); !inserted) {
        return std::unexpected(std::format(
            "{} and {} would both be written to {}", it->second.string(),
            input.string(), output.string()));
      }
      jobs.push_back({input, output});
    }
  }

  for (const Job &job : jobs) {
    std::error_code error;
    if (std::filesystem::exists(job.output, error)) {
      if (std::filesystem::equivalent(job.input, job.output, error)) {
        return std::unexpected(std::format(
            "output {} is the input file itself", job.output.string()));
      }
      if (!command_line.force) {
        return std::unexpected(std::format(
            "{} already exists (use --force to overwrite)", job.output.string()));
      }
    }
  }
  return jobs;
}

} // namespace detail

int run(std::span<const std::string_view> arguments, std::ostream &output,
        std::ostream &error) {
  const auto command_line = detail::parse_command_line(arguments);
  if (!command_line) {
    error << "spectrallab: " << command_line.error() << '\n'
          << "Try 'spectrallab --help' for more information.\n";
    return exit_usage;
  }
  if (command_line->help) {
    output << usage_text;
    return exit_success;
  }
  if (command_line->version) {
    output << "spectrallab " << ircore::version() << '\n';
    return exit_success;
  }
  const auto jobs = detail::plan_jobs(*command_line);
  if (!jobs) {
    error << "spectrallab: " << jobs.error() << '\n'
          << "Try 'spectrallab --help' for more information.\n";
    return exit_usage;
  }
  if (command_line->output_dir && !command_line->analyze) {
    std::error_code status;
    std::filesystem::create_directories(*command_line->output_dir, status);
    if (status) {
      error << std::format("spectrallab: cannot create {}: {}\n",
                           command_line->output_dir->string(),
                           status.message());
      return exit_failure;
    }
  }

  int failures = 0;
  for (const detail::Job &job : *jobs) {
    const auto report =
        command_line->analyze
            ? ircore::analyze_file(job.input, command_line->options)
            : ircore::process_file(job.input, job.output,
                                   command_line->options);
    if (!report) {
      ++failures;
      error << std::format("spectrallab: {}: {}\n", job.input.string(),
                           report.error().message);
      continue;
    }
    if (!command_line->quiet) {
      if (command_line->analyze) {
        output << job.input.string() << '\n';
      } else {
        output << job.input.string() << " -> " << job.output.string() << '\n';
      }
      print_report(output, *report);
    }
  }
  if (failures != 0 && jobs->size() > 1) {
    error << std::format("spectrallab: {} of {} files failed\n", failures,
                         jobs->size());
  }
  return failures == 0 ? exit_success : exit_failure;
}

} // namespace ircli

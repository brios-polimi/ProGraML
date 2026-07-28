// Copyright 2019-2020 the ProGraML authors.
//
// Contact Chris Cummins <chrisc.101@gmail.com>.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "programl/graph/format/node_link_graph.h"

#include "labm8/cpp/logging.h"
#include "labm8/cpp/status.h"
#include "labm8/cpp/status_macros.h"
#include "nlohmann/json.hpp"
#include "programl/proto/program_graph.pb.h"

using json = nlohmann::json;
using labm8::Status;

namespace programl {
namespace graph {
namespace format {

namespace detail {

template <typename T>
[[nodiscard]] Status MaybeAddFeatures(const T& message, json* dict) {
  if (message.has_features()) {
    auto features = json::object({});
    RETURN_IF_ERROR(CreateFeaturesDict(message.features(), &features));
    (*dict)["features"] = features;
  }

  return Status::OK;
}

Status CreateGraphDict(const ProgramGraph& graph, json* graphDict) {
  auto functions = json::array({});
  for (int i = 0; i < graph.function_size(); ++i) {
    const Function& function = graph.function(i);
    auto functionDict = json::object({
        {"name", function.name()},
        {"module", function.module()},
    });
    RETURN_IF_ERROR(MaybeAddFeatures(function, &functionDict));
    functions.push_back(functionDict);
  }
  (*graphDict)["function"] = functions;

  auto modules = json::array({});
  for (int i = 0; i < graph.module_size(); ++i) {
    const Module& module = graph.module(i);
    auto moduleDict = json::object({
        {"name", module.name()},
    });
    RETURN_IF_ERROR(MaybeAddFeatures(module, &moduleDict));
    modules.push_back(moduleDict);
  }
  (*graphDict)["module"] = modules;

  RETURN_IF_ERROR(MaybeAddFeatures(graph, graphDict));

  return Status::OK;
}

Status CreateNodeDict(const Node& node, int id, json* nodeDict) {
  *nodeDict = json::object({
      {"id", id},
      {"type", node.type()},
      {"text", node.text()},
      {"function", node.function()},
      {"block", node.block()},
  });
  return MaybeAddFeatures(node, nodeDict);
}

Status CreateNodesList(const ProgramGraph& graph, json* nodes) {
  for (int i = 0; i < graph.node_size(); ++i) {
    auto nodeDict = json::object();
    RETURN_IF_ERROR(CreateNodeDict(graph.node(i), i, &nodeDict));
    nodes->push_back(nodeDict);
  }

  return Status::OK;
}

Status CreateEdgeDict(const Edge& edge, json* edgeDict) {
  *edgeDict = json::object({
      {"flow", edge.flow()},
      {"position", edge.position()},
      {"source", edge.source()},
      {"target", edge.target()},
      {"key", 0},
  });
  return MaybeAddFeatures(edge, edgeDict);
}

Status CreateLinksList(const ProgramGraph& graph, json* edges) {
  for (int i = 0; i < graph.edge_size(); ++i) {
    auto edgeDict = json::object();
    RETURN_IF_ERROR(CreateEdgeDict(graph.edge(i), &edgeDict));
    edges->push_back(edgeDict);
  }
  return Status::OK;
}

Status CreateFeatureArray(const Feature& feature, json* featureArray) {
  if (feature.has_bytes_list()) {
    for (int i = 0; i < feature.bytes_list().value_size(); ++i) {
      featureArray->push_back(feature.bytes_list().value(i));
    }
  } else if (feature.has_float_list()) {
    for (int i = 0; i < feature.float_list().value_size(); ++i) {
      featureArray->push_back(feature.float_list().value(i));
    }
  } else if (feature.has_int64_list()) {
    for (int i = 0; i < feature.int64_list().value_size(); ++i) {
      featureArray->push_back(feature.int64_list().value(i));
    }
  }

  return Status::OK;
}

Status CreateFeaturesDict(const Features& features, json* featuresDict) {
  for (const auto& feature : features.feature()) {
    auto featureArray = json::array({});
    RETURN_IF_ERROR(CreateFeatureArray(feature.second, &featureArray));
    featuresDict->push_back({feature.first, featureArray});
  }
  return Status::OK;
}

}  // namespace detail

Status ProgramGraphToNodeLinkGraph(const ProgramGraph& graph, json* dict) {
  CHECK(dict) << "nullptr for output argument";

  (*dict)["directed"] = true;
  (*dict)["multigraph"] = true;
  (*dict)["graph"] = json::object();
  (*dict)["nodes"] = json::array();
  (*dict)["links"] = json::array();

  RETURN_IF_ERROR(detail::CreateGraphDict(graph, &(*dict)["graph"]));
  RETURN_IF_ERROR(detail::CreateNodesList(graph, &(*dict)["nodes"]));
  RETURN_IF_ERROR(detail::CreateLinksList(graph, &(*dict)["links"]));

  return Status::OK;
}

Status WriteProgramGraphNodeLinkJson(const ProgramGraph& graph,
                                     std::ostream* output) {
  CHECK(output) << "nullptr for output argument";

  auto graphDict = json::object();
  RETURN_IF_ERROR(detail::CreateGraphDict(graph, &graphDict));
  *output << R"({"directed":true,"graph":)" << graphDict
          << R"(,"links":[)";
  for (int i = 0; i < graph.edge_size(); ++i) {
    if (i) {
      *output << ',';
    }
    auto edgeDict = json::object();
    RETURN_IF_ERROR(detail::CreateEdgeDict(graph.edge(i), &edgeDict));
    *output << edgeDict;
  }
  *output << R"(],"multigraph":true,"nodes":[)";
  for (int i = 0; i < graph.node_size(); ++i) {
    if (i) {
      *output << ',';
    }
    auto nodeDict = json::object();
    RETURN_IF_ERROR(detail::CreateNodeDict(graph.node(i), i, &nodeDict));
    *output << nodeDict;
  }
  *output << "]}";
  return Status::OK;
}

}  // namespace format
}  // namespace graph
}  // namespace programl

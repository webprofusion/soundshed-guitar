#pragma once

#include "presets/PresetTypes.h"
#include "presets/PresetTypesV2.h"
#include <optional>
#include <chrono>

namespace namguitar
{
  /**
   * Utility class for migrating v1 Preset format to PresetV2 format.
   * 
   * Migration strategy:
   * - Creates a linear signal graph from the old fxChain
   * - Converts attachments to ResourceRef format
   * - Maps v1 parameters to appropriate node parameters
   */
  class PresetMigration
  {
  public:
    /**
     * Migrate a v1 Preset to v2 format.
     * @param v1 The original v1 preset
     * @return The migrated v2 preset
     */
    static PresetV2 MigrateToV2(const Preset& v1)
    {
      PresetV2 v2;

      // Copy basic metadata
      v2.id = v1.id;
      v2.name = v1.name;
      v2.category = v1.category;
      v2.description = v1.description;
      v2.author = "";  // Not available in v1
      v2.version = "2.0";
      v2.formatVersion = 2;
      v2.createdAt = GetCurrentTimestamp();
      v2.modifiedAt = v2.createdAt;

      // Initialize default global settings
      v2.globals.inputTrim = 0.0;
      v2.globals.outputTrim = 0.0;
      v2.globals.masterVolume = 1.0;

      // Look up parameter values from v1
      auto findParam = [&v1](const std::string& id) -> std::optional<double> {
        for (const auto& p : v1.parameters)
        {
          if (p.id == id)
            return p.value;
        }
        return std::nullopt;
      };

      // Copy relevant global parameters
      if (auto val = findParam("inputTrim"))
        v2.globals.inputTrim = *val;
      if (auto val = findParam("outputTrim"))
        v2.globals.outputTrim = *val;

      // Build the signal graph
      BuildSignalGraph(v1, v2);

      // Handle embedded resources from inline attachments
      ConvertAttachments(v1, v2);

      return v2;
    }

    /**
     * Check if a preset needs migration.
     */
    static bool NeedsMigration(int formatVersion)
    {
      return formatVersion < 2;
    }

  private:
    static std::string GetCurrentTimestamp()
    {
      auto now = std::chrono::system_clock::now();
      auto time = std::chrono::system_clock::to_time_t(now);
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time));
      return buf;
    }

    static void BuildSignalGraph(const Preset& v1, PresetV2& v2)
    {
      // Create a linear signal chain based on v1 configuration
      // V1 chain: Input -> Gate -> Amp -> Cab -> EQ -> Delay -> Reverb -> Output

      std::string prevNode = GraphNode::NODE_INPUT;
      int nodeIndex = 0;

      auto findParam = [&v1](const std::string& id) -> std::optional<double> {
        for (const auto& p : v1.parameters)
        {
          if (p.id == id)
            return p.value;
        }
        return std::nullopt;
      };

      auto addEdge = [&v2](const std::string& from, const std::string& to) {
        GraphEdge edge;
        edge.from = from;
        edge.to = to;
        edge.fromPort = 0;
        edge.toPort = 0;
        edge.gain = 1.0;
        v2.graph.edges.push_back(edge);
      };

      // Add noise gate if enabled
      if (auto gateEnabled = findParam("gateEnabled"); gateEnabled && *gateEnabled > 0.5)
      {
        GraphNode gate;
        gate.id = "gate_" + std::to_string(nodeIndex++);
        gate.type = "dynamics_gate";
        gate.displayName = "Noise Gate";
        gate.category = "dynamics";
        gate.bypassed = false;

        if (auto val = findParam("gateThreshold"))
          gate.params["threshold"] = *val;

        v2.graph.nodes.push_back(gate);
        addEdge(prevNode, gate.id);
        prevNode = gate.id;
      }

      // Add NAM amp model
      bool hasAmp = false;
      for (const auto& att : v1.attachments)
      {
        if (att.type == "nam")
        {
          hasAmp = true;
          break;
        }
      }
      if (!hasAmp && !v1.audioFxModelId.empty())
        hasAmp = true;

      if (hasAmp)
      {
        GraphNode amp;
        amp.id = "amp_" + std::to_string(nodeIndex++);
        amp.type = "amp_nam";
        amp.displayName = "Amp";
        amp.category = "amp";
        amp.bypassed = false;

        // Set up resource reference
        for (const auto& att : v1.attachments)
        {
          if (att.type == "nam")
          {
            if (!att.data.empty())
            {
              // Inline embedded data - will be stored in v2.embeddedResources
              amp.resource.embeddedId = "nam_" + std::to_string(nodeIndex);
            }
            else if (!att.filePath.empty())
            {
              amp.resource.filePath = att.filePath;
            }
            else if (!att.id.empty())
            {
              amp.resource.type = "nam";
              amp.resource.id = att.id;
            }
            break;
          }
        }

        // Fallback to library ID
        if (!amp.resource.isSet() && !v1.audioFxModelId.empty())
        {
          amp.resource.type = "nam";
          amp.resource.id = v1.audioFxModelId;
        }

        if (auto val = findParam("drive"))
          amp.params["inputGain"] = *val * 24.0 - 12.0;  // Normalize to dB range

        v2.graph.nodes.push_back(amp);
        addEdge(prevNode, amp.id);
        prevNode = amp.id;
      }

      // Add cabinet (IR or simple)
      bool hasIR = false;
      for (const auto& att : v1.attachments)
      {
        if (att.type == "ir")
        {
          hasIR = true;
          break;
        }
      }
      if (!hasIR && !v1.irId.empty())
        hasIR = true;

      bool simpleCabEnabled = false;
      if (auto val = findParam("simpleCabEnabled"))
        simpleCabEnabled = *val > 0.5;

      if (hasIR)
      {
        GraphNode cab;
        cab.id = "cab_" + std::to_string(nodeIndex++);
        cab.type = "cab_ir";
        cab.displayName = "Cabinet";
        cab.category = "cab";
        cab.bypassed = false;

        for (const auto& att : v1.attachments)
        {
          if (att.type == "ir")
          {
            if (!att.data.empty())
            {
              cab.resource.embeddedId = "ir_" + std::to_string(nodeIndex);
            }
            else if (!att.filePath.empty())
            {
              cab.resource.filePath = att.filePath;
            }
            else if (!att.id.empty())
            {
              cab.resource.type = "ir";
              cab.resource.id = att.id;
            }
            break;
          }
        }

        if (!cab.resource.isSet() && !v1.irId.empty())
        {
          cab.resource.type = "ir";
          cab.resource.id = v1.irId;
        }

        v2.graph.nodes.push_back(cab);
        addEdge(prevNode, cab.id);
        prevNode = cab.id;
      }
      else if (simpleCabEnabled)
      {
        GraphNode cab;
        cab.id = "cab_" + std::to_string(nodeIndex++);
        cab.type = "cab_simple";
        cab.displayName = "Simple Cab";
        cab.category = "cab";
        cab.bypassed = false;

        if (auto val = findParam("simpleCabBass"))
          cab.params["bass"] = *val;
        if (auto val = findParam("simpleCabPresence"))
          cab.params["presence"] = *val;
        if (auto val = findParam("simpleCabBrightness"))
          cab.params["brightness"] = *val;

        v2.graph.nodes.push_back(cab);
        addEdge(prevNode, cab.id);
        prevNode = cab.id;
      }

      // Add parametric EQ if enabled
      if (auto eqEnabled = findParam("eqEnabled"); eqEnabled && *eqEnabled > 0.5)
      {
        GraphNode eq;
        eq.id = "eq_" + std::to_string(nodeIndex++);
        eq.type = "eq_parametric";
        eq.displayName = "EQ";
        eq.category = "eq";
        eq.bypassed = false;

        // Copy EQ band parameters
        for (int band = 0; band < 4; ++band)
        {
          std::string prefix = "eq" + std::to_string(band);
          if (auto val = findParam(prefix + "Gain"))
            eq.params[prefix + "_gain"] = *val;
          if (auto val = findParam(prefix + "Freq"))
            eq.params[prefix + "_freq"] = *val;
          if (auto val = findParam(prefix + "Q"))
            eq.params[prefix + "_q"] = *val;
        }

        v2.graph.nodes.push_back(eq);
        addEdge(prevNode, eq.id);
        prevNode = eq.id;
      }

      // Add delay if enabled
      if (auto delayEnabled = findParam("delayEnabled"); delayEnabled && *delayEnabled > 0.5)
      {
        GraphNode delay;
        delay.id = "delay_" + std::to_string(nodeIndex++);
        delay.type = "delay_digital";
        delay.displayName = "Delay";
        delay.category = "delay";
        delay.bypassed = false;

        if (auto val = findParam("delayTime"))
          delay.params["time"] = *val;
        if (auto val = findParam("delayFeedback"))
          delay.params["feedback"] = *val;
        if (auto val = findParam("delayMix"))
          delay.params["mix"] = *val;

        v2.graph.nodes.push_back(delay);
        addEdge(prevNode, delay.id);
        prevNode = delay.id;
      }

      // Add reverb if enabled
      if (auto reverbEnabled = findParam("reverbEnabled"); reverbEnabled && *reverbEnabled > 0.5)
      {
        GraphNode reverb;
        reverb.id = "reverb_" + std::to_string(nodeIndex++);
        reverb.type = "reverb_room";
        reverb.displayName = "Reverb";
        reverb.category = "reverb";
        reverb.bypassed = false;

        if (auto val = findParam("reverbDecay"))
          reverb.params["decay"] = *val;
        if (auto val = findParam("reverbDamping"))
          reverb.params["damping"] = *val;
        if (auto val = findParam("reverbMix"))
          reverb.params["mix"] = *val;

        v2.graph.nodes.push_back(reverb);
        addEdge(prevNode, reverb.id);
        prevNode = reverb.id;
      }

      // Connect final node to output
      addEdge(prevNode, GraphNode::NODE_OUTPUT);
    }

    static void ConvertAttachments(const Preset& v1, PresetV2& v2)
    {
      int index = 0;
      for (const auto& att : v1.attachments)
      {
        if (!att.data.empty())
        {
          EmbeddedResource embedded;
          embedded.id = att.type + "_" + std::to_string(index++);
          embedded.type = att.type;
          embedded.name = att.id.empty() ? embedded.id : att.id;
          embedded.data = att.data;
          embedded.hash = att.hash;
          v2.embeddedResources.push_back(embedded);
        }
      }
    }
  };

} // namespace namguitar

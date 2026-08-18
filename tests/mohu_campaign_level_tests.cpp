#include "mohu/campaign_level.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error{message};
}

void testEveryCampaignDirectory() {
  for (std::size_t index{}; index < mohu::campaign_level_directories.size();
       ++index) {
    const auto path = std::string{mohu::campaign_level_directories[index]} +
                      "/STREAM/ANY.BIN";
    require(mohu::campaignLevelForDiscPath(path) == index + 1U,
            "Campaign directory mapped to the wrong level");
  }
}

void testEveryAssetInLevelActivatesIt() {
  require(mohu::campaignLevelForDiscPath("DATA/MSN2/LVL1/2_1.BSD") == 1U,
          "BSD did not activate its level");
  require(mohu::campaignLevelForDiscPath("DATA/MSN2/LVL1/2_10.TAF") == 1U,
          "Texture archive did not activate its level");
  require(mohu::campaignLevelForDiscPath("DATA/MSN2/LVL1/M2_1_1.VB") == 1U,
          "Audio bank did not activate its level");
  require(mohu::campaignLevelForDiscPath("DATA/MSN8/LVL4/8_4.BSD") == 24U,
          "Liberation mission did not activate its exact level");
}

void testDirectoryBoundariesAreExact() {
  require(mohu::campaignLevelForDiscPath("DATA/MSN2/LVL10/FAKE.BSD") == 0U,
          "LVL1 matched the LVL10 sibling");
  require(mohu::campaignLevelForDiscPath("DATA/MSN2/LVL1") == 0U,
          "Directory without a file was accepted as CD data");
  require(mohu::campaignLevelForDiscPath("DATA/SHELL/SHELL.BIN") == 0U,
          "Frontend data activated campaign lighting");
}

} // namespace

int main() {
  try {
    testEveryCampaignDirectory();
    testEveryAssetInLevelActivatesIt();
    testDirectoryBoundariesAreExact();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "MOHU campaign-level tests passed\n";
  return 0;
}

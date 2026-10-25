import { describe, expect, it } from "vitest";

import { normalizeSearchResults } from "../ts/jam/search.js";
import { decodeYouTubeText, normalizeFavorites } from "../ts/jam/state.js";

describe("YouTube titles", () => {
  it("decodes the entities the search API escapes", () => {
    expect(decodeYouTubeText("80&#39;s Rock &amp; Roll &quot;Live&quot; &lt;3 &#x2665;")).toBe("80's Rock & Roll \"Live\" <3 ♥");
    expect(decodeYouTubeText("Plain title")).toBe("Plain title");
    expect(decodeYouTubeText("&unknown; &#0;")).toBe("&unknown; &#0;");
  });

  it("decodes new search results and stored favourites", () => {
    const results = normalizeSearchResults({
      items: [{ id: { videoId: "a" }, snippet: { title: "80&#39;s Rock", channelTitle: "Tom &amp; Jerry", thumbnails: {} } }],
    });
    expect(results[0]).toMatchObject({ title: "80's Rock", channelTitle: "Tom & Jerry" });

    const favorites = normalizeFavorites([{ videoId: "b", title: "It&#39;s", channelTitle: "C", thumbnailUrl: "" }, { videoId: 1 }]);
    expect(favorites).toEqual([{ videoId: "b", title: "It's", channelTitle: "C", thumbnailUrl: "" }]);
  });
});

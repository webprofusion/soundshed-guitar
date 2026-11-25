/**
 * Shapes the tone sharing API returns, and the shapes this panel keeps locally.
 *
 * Types only, so importing this costs nothing at runtime and it can sit at the
 * bottom of the feature's dependency graph.
 */

export type ToneSharingUser = {
  id: string;
  email: string;
  role: string;
  displayName?: string | null;
  bio?: string | null;
  creatorProfileHandle?: string | null;
  avatarAssetId?: string | null;
  avatarUrl?: string | null;
};

/** A moderator's latest note on a tone: only ever sent to its creator and to admins. */
export type ToneSharingModerationNote = {
  action: string;
  notes: string;
  createdAt: string;
};

/** What `/admin/creators/:userId` says about a creator (admins only). */
export type ToneSharingCreatorModeration = {
  userId: string;
  email: string;
  displayName: string | null;
  handle: string | null;
  /** Everything they publish waits in the Review Queue. */
  requiresReview: boolean;
  notes: string | null;
  setAt: string | null;
  setBy: string | null;
  /** How many of their presets and packs are in each moderation status. */
  items: Record<string, number>;
  packs: Record<string, number>;
};

export type ToneSharingItem = {
  id: string;
  title: string;
  type: string;
  featured?: boolean;
  creatorUserId?: string | null;
  moderationStatus?: string;
  moderationNote?: ToneSharingModerationNote | null;
  /** The creator's local preset this was published from (only sent to its creator). */
  sourcePresetId?: string | null;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  creatorBio?: string | null;
  creatorAvatarUrl?: string | null;
  favoriteCount?: number;
  ratingCount?: number;
  averageRating?: number | null;
  currentUserFavorite?: boolean;
  currentUserRating?: number | null;
  downloadCount?: number;
  downloadsCount?: number;
  downloads_count?: number;
  description?: string | null;
  tags?: string[] | null;
};

export type ToneSharingPack = {
  id: string;
  title: string;
  featured?: boolean;
  creatorUserId?: string | null;
  moderationStatus?: string;
  moderationNote?: ToneSharingModerationNote | null;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  creatorBio?: string | null;
  creatorAvatarUrl?: string | null;
  description?: string | null;
  thumbnailUrl?: string | null;
  thumbnailAssetId?: string | null;
};

export type ToneSharingPackDetails = {
  pack: ToneSharingPack;
  items: Array<{ itemId: string; sortOrder: number; title: string; type: string; moderationStatus?: string; description?: string | null; tags?: string[] | null }>;
};

export type ShareConsentStatus = {
  consentType: string;
  version: number;
  accepted: boolean;
  acceptedAt: string | null;
};

export type ToneSharingRow = {
  id: string;
  slug: string;
  title: string;
  /** One line that scrolls sideways rather than a grid that wraps: the featured packs over Presets. */
  strip?: boolean;
  items: Array<{
    id: string;
    kind: "item" | "pack";
    title: string;
    type: string | null;
    featured?: boolean;
    moderationStatus?: string;
    moderationNote?: ToneSharingModerationNote | null;
    creatorEmail?: string | null;
    creatorDisplayName?: string | null;
    creatorHandle?: string | null;
    creatorProfileHandle?: string | null;
    profileHandle?: string | null;
    creatorBio?: string | null;
    creatorAvatarUrl?: string | null;
    favoriteCount?: number;
    ratingCount?: number;
    averageRating?: number | null;
    currentUserFavorite?: boolean;
    currentUserRating?: number | null;
    downloadCount?: number;
    description?: string | null;
    tags?: string[] | null;
    thumbnailUrl?: string | null;
    thumbnailAssetId?: string | null;
  }>;
};

export type InstalledPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

export type InstalledPackResourceRef = {
  type: string;
  id: string;
};

export type InstalledPackMetadata = {
  id: string;
  title: string;
  source: InstalledPackSource;
  importedAt: string;
  packId?: string;
  archivePath?: string;
  archiveFileName?: string;
  presetIds: string[];
  presetSignatures?: Record<string, string>;
  resources: InstalledPackResourceRef[];
  /** Blends the engine's install added (it removes them with the pack). */
  blends?: string[];
};

export type ToneSharingShareTarget = {
  kind: "item" | "pack";
  id: string;
};

/** Which feed the browse area is showing. */
/** "items" is the Presets section: the community presets, with the featured packs over them while nothing is searched. */
export type BrowseMode = "items" | "packs" | "installed" | "mine" | "review";

export type BrowseCollectionState = {
  page: number;
  pageSize: number;
  hasMore: boolean;
  loadingMore: boolean;
  items: ToneSharingItem[];
  packs: ToneSharingPack[];
  /** The /home rows of featured packs that Presets shows over its results while nothing is searched. */
  featuredRows: ToneSharingRow[];
};

export type ToneActionIcon = "preview" | "download" | "share" | "view" | "approve" | "reject" | "edit" | "delete" | "feature";

export type InstalledToneSharingLookup = {
  itemIds: Set<string>;
  packIds: Set<string>;
};

export type ResizedImageResult = {
  blob: Blob;
  width: number;
  height: number;
};

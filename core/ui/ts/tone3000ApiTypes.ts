export type Tone3000Architecture = "1" | "2" | "custom";

export type Tone3000Platform = "nam" | "ir" | "aida-x" | "aa-snapshot" | "proteus";

export type Tone3000Gear = "amp" | "full-rig" | "pedal" | "outboard" | "ir";

export type Tone3000License =
  | "t3k"
  | "cc-by"
  | "cc-by-sa"
  | "cc-by-nc"
  | "cc-by-nc-sa"
  | "cc-by-nd"
  | "cc-by-nc-nd"
  | "cco";

export type Tone3000Size = "standard" | "lite" | "feather" | "nano" | "custom";

export type Tone3000UsersSort = "tones" | "downloads" | "favorites" | "models";

export type Tone3000TonesSort = "best-match" | "newest" | "oldest" | "trending" | "downloads-all-time";

export interface Tone3000ApiSession {
  access_token: string;
  refresh_token?: string;
  expires_in: number;
  token_type?: "bearer";
  scope?: string;
}

export interface Tone3000EmbeddedUser {
  id: number | string;
  username: string;
  avatar_url?: string | null;
  url?: string;
  name?: string;
  display_name?: string;
}

export interface Tone3000PublicUser extends Tone3000EmbeddedUser {
  bio?: string | null;
  links?: string[] | null;
  downloads_count?: number;
  favorites_count?: number;
  models_count?: number;
  tones_count?: number;
}

export interface Tone3000Tag {
  id?: number;
  name?: string;
}

export interface Tone3000Make {
  id?: number;
  name?: string;
}

export interface Tone3000Tone {
  id: number | string;
  user_id?: number;
  user?: Tone3000EmbeddedUser;
  created_at?: string;
  updated_at?: string;
  title: string;
  name?: string;
  slug?: string;
  description?: string | null;
  gear?: Tone3000Gear | (string & {});
  platform?: Tone3000Platform | (string & {});
  license?: Tone3000License;
  sizes?: Tone3000Size[];
  makes?: Tone3000Make[];
  tags?: Tone3000Tag[];
  images?: string[] | null;
  links?: string[] | null;
  is_public?: boolean | null;
  models_count?: number;
  a1_models_count?: number;
  a2_models_count?: number;
  custom_models_count?: number;
  irs_count?: number;
  downloads_count?: number;
  favorites_count?: number;
  url?: string;
  equipment_image_url?: string;
  equipment_image?: string;
  gear_image_url?: string;
  image_url?: string;
  thumbnail_url?: string;
}

export interface Tone3000Model {
  id: number | string;
  created_at?: string;
  updated_at?: string;
  user_id?: number;
  model_url: string;
  name: string;
  size?: Tone3000Size;
  tone_id?: number;
  architecture_version?: Tone3000Architecture | null;
}

export interface Tone3000PaginatedResponse<T> {
  data: T[];
  page: number;
  page_size: number;
  total: number;
  total_pages: number;
}

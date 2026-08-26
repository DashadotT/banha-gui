-- =============================================================================
-- BANHA — SUPABASE DATABASE SCHEMA
-- UPDATED FOR NODE 2 + START/STOP RECORDING FLOW
-- =============================================================================
--
-- FLOW:
--
-- Node 1
-- NODE:1,TYPE:START
--        |
--        v
-- Node 2 inserts recording
--        |
--        v
-- recordings:
-- id               = UUID
-- started_at       = automatically now()
-- ended_at         = NULL
-- duration_seconds = NULL
-- status           = recording
--        |
--        v
-- Node 1 sends DATA packets
-- PACKET:1
-- PACKET:2
-- PACKET:3
--        |
--        v
-- Node 2 inserts environmental_readings
--        |
--        v
-- Node 1
-- NODE:1,TYPE:STOP
--        |
--        v
-- Node 2 PATCH:
-- duration_seconds = calculated value
-- status           = pending_assessment
--        |
--        v
-- DATABASE TRIGGER:
-- ended_at = now()
--        |
--        v
-- Dashboard adds assessment
--        |
--        v
-- recording.status = completed
--
-- =============================================================================


-- =============================================================================
-- EXTENSION
-- =============================================================================

create extension if not exists "uuid-ossp";


-- =============================================================================
-- PROFILES
-- =============================================================================

create table if not exists public.profiles (
  id uuid primary key references auth.users(id) on delete cascade,

  full_name text not null default 'New User',

  email text not null,

  role text not null default 'researcher'
    check (
      role in (
        'administrator',
        'researcher'
      )
    ),

  status text not null default 'active'
    check (
      status in (
        'active',
        'inactive'
      )
    ),

  created_at timestamptz not null default now()
);


-- =============================================================================
-- AUTOMATIC PROFILE CREATION
-- =============================================================================

create or replace function public.handle_new_user()
returns trigger
language plpgsql
security definer
as $$
begin

  insert into public.profiles (
    id,
    full_name,
    email
  )
  values (
    new.id,
    coalesce(
      new.raw_user_meta_data ->> 'full_name',
      new.email
    ),
    new.email
  );

  return new;

end;
$$;


drop trigger if exists on_auth_user_created
on auth.users;


create trigger on_auth_user_created
after insert on auth.users
for each row
execute procedure public.handle_new_user();


-- =============================================================================
-- DEVICES
-- =============================================================================

create table if not exists public.devices (
  id uuid primary key default uuid_generate_v4(),

  device_name text not null,

  node_number integer not null
    check (node_number >= 1),

  status text not null default 'offline'
    check (
      status in (
        'online',
        'offline',
        'recording'
      )
    ),

  created_at timestamptz not null default now()
);


create index if not exists idx_devices_node_number
on public.devices(node_number);


-- =============================================================================
-- RECORDINGS
--
-- Created when Node 2 receives:
-- NODE:1,TYPE:START
-- =============================================================================

create table if not exists public.recordings (
  id uuid primary key default uuid_generate_v4(),

  device_id uuid
    references public.devices(id)
    on delete set null,

  -- Automatically set when Node 2 creates the recording
  started_at timestamptz not null default now(),

  -- Automatically set by trigger when recording stops
  ended_at timestamptz,

  -- Sent by Node 2 when STOP is received
  duration_seconds integer,

  status text not null default 'recording'
    check (
      status in (
        'recording',
        'pending_assessment',
        'completed'
      )
    ),

  is_archived boolean not null default false,

  archived_at timestamptz,

  created_at timestamptz not null default now(),

  -- End time cannot be before start time
  check (
    ended_at is null
    or ended_at >= started_at
  ),

  -- Duration cannot be negative
  check (
    duration_seconds is null
    or duration_seconds >= 0
  )
);


create index if not exists idx_recordings_status
on public.recordings(
  status,
  is_archived
);


create index if not exists idx_recordings_device
on public.recordings(device_id);


create index if not exists idx_recordings_started_at
on public.recordings(started_at desc);


-- =============================================================================
-- AUTOMATICALLY SET ENDED_AT
--
-- When Node 2 changes:
--
-- recording -> pending_assessment
--
-- Supabase automatically saves:
--
-- ended_at = now()
-- =============================================================================

create or replace function public.set_recording_end_time()
returns trigger
language plpgsql
security definer
as $$
begin

  -- Only execute when a recording is being stopped

  if
    old.status = 'recording'
    and new.status in (
      'pending_assessment',
      'completed'
    )
    and new.ended_at is null
  then

    new.ended_at = now();

  end if;


  return new;

end;
$$;


drop trigger if exists before_recording_stop
on public.recordings;


create trigger before_recording_stop
before update
on public.recordings
for each row
execute function public.set_recording_end_time();


-- =============================================================================
-- ENVIRONMENTAL READINGS
--
-- One row per approximately 1-minute average.
--
-- PACKET NUMBERS:
--
-- Recording A:
-- Packet 1
-- Packet 2
-- Packet 3
--
-- Recording B:
-- Packet 1
-- Packet 2
-- Packet 3
--
-- This works because uniqueness is:
--
-- unique(recording_id, packet_number)
-- =============================================================================

create table if not exists public.environmental_readings (
  id uuid primary key default uuid_generate_v4(),

  recording_id uuid not null
    references public.recordings(id)
    on delete cascade,

  packet_number integer not null
    check (
      packet_number >= 1
    ),

  average_co2 numeric(8, 2) not null
    check (
      average_co2 >= 0
    ),

  average_temperature numeric(6, 2) not null,

  average_noise numeric(6, 2) not null
    check (
      average_noise >= 0
      and average_noise <= 150
    ),

  recorded_at timestamptz not null default now(),

  created_at timestamptz not null default now(),

  -- Packet 1 can exist in multiple recordings,
  -- but only once within the same recording.
  unique (
    recording_id,
    packet_number
  )
);


create index if not exists idx_readings_recording
on public.environmental_readings(
  recording_id,
  packet_number
);


create index if not exists idx_readings_recorded_at
on public.environmental_readings(
  recorded_at desc
);


-- =============================================================================
-- ASSESSMENTS
-- =============================================================================

create table if not exists public.assessments (
  id uuid primary key default uuid_generate_v4(),

  recording_id uuid not null
    references public.recordings(id)
    on delete cascade,

  subject text not null,

  section text not null,

  group_type text not null
    check (
      group_type in (
        'Experimental',
        'Comparison'
      )
    ),

  assessment_type text not null,

  assessment_number integer not null
    check (
      assessment_number >= 1
    ),

  assessment_date date not null,

  class_average_score numeric(6, 2) not null
    check (
      class_average_score >= 0
    ),

  total_possible_score numeric(6, 2) not null
    check (
      total_possible_score > 0
    ),

  score_percentage numeric(5, 2) not null
    check (
      score_percentage >= 0
      and score_percentage <= 100
    ),

  is_archived boolean not null default false,

  archived_at timestamptz,

  created_at timestamptz not null default now()
);


create index if not exists idx_assessments_recording
on public.assessments(recording_id);


create index if not exists idx_assessments_subject
on public.assessments(
  subject,
  section
);


-- =============================================================================
-- WHEN ASSESSMENT IS ADDED
--
-- pending_assessment -> completed
-- =============================================================================

create or replace function public.mark_recording_assessed()
returns trigger
language plpgsql
security definer
as $$
begin

  update public.recordings

  set
    status = 'completed'

  where
    id = new.recording_id
    and status = 'pending_assessment';


  return new;

end;
$$;


drop trigger if exists on_assessment_created
on public.assessments;


create trigger on_assessment_created
after insert on public.assessments
for each row
execute function public.mark_recording_assessed();


-- =============================================================================
-- SETTINGS OPTIONS
-- =============================================================================

create table if not exists public.settings_options (
  id uuid primary key default uuid_generate_v4(),

  category text not null
    check (
      category in (
        'subject',
        'section',
        'assessment_type'
      )
    ),

  value text not null,

  is_archived boolean not null default false,

  archived_at timestamptz,

  created_at timestamptz not null default now(),

  unique (
    category,
    value
  )
);


create index if not exists idx_settings_options_category
on public.settings_options(
  category,
  is_archived
);


-- =============================================================================
-- DEFAULT SETTINGS
-- =============================================================================

insert into public.settings_options (
  category,
  value
)
values

  -- Subjects

  (
    'subject',
    'Information Management'
  ),

  (
    'subject',
    'System Integration and Architecture'
  ),


  -- Sections

  (
    'section',
    'A'
  ),

  (
    'section',
    'B'
  ),

  (
    'section',
    'C'
  ),


  -- Assessment Types

  (
    'assessment_type',
    'Quiz'
  ),

  (
    'assessment_type',
    'Examination'
  ),

  (
    'assessment_type',
    'Activity'
  ),

  (
    'assessment_type',
    'Exercise'
  )

on conflict (
  category,
  value
)
do nothing;


-- =============================================================================
-- TEST DEVICE
--
-- MUST MATCH DEVICE_ID IN NODE 2:
--
-- 11111111-1111-1111-1111-111111111111
-- =============================================================================

insert into public.devices (
  id,
  device_name,
  node_number,
  status
)
values (
  '11111111-1111-1111-1111-111111111111',
  'BANHA Node 1 Sensor Device',
  1,
  'online'
)

on conflict (id)
do update

set
  device_name = excluded.device_name,
  node_number = excluded.node_number,
  status = excluded.status;


-- =============================================================================
-- ENABLE ROW LEVEL SECURITY
-- =============================================================================

alter table public.profiles
enable row level security;


alter table public.devices
enable row level security;


alter table public.recordings
enable row level security;


alter table public.environmental_readings
enable row level security;


alter table public.assessments
enable row level security;


alter table public.settings_options
enable row level security;


-- =============================================================================
-- REMOVE OLD POLICIES
--
-- This prevents errors when running the schema again.
-- =============================================================================

drop policy if exists "Authenticated users can view profiles"
on public.profiles;

drop policy if exists "Users can update their own profile"
on public.profiles;

drop policy if exists "Authenticated users can view devices"
on public.devices;

drop policy if exists "Authenticated users can manage recordings"
on public.recordings;

drop policy if exists "Authenticated users can manage readings"
on public.environmental_readings;

drop policy if exists "Authenticated users can manage assessments"
on public.assessments;

drop policy if exists "Authenticated users can manage settings options"
on public.settings_options;

drop policy if exists "IoT can view devices"
on public.devices;

drop policy if exists "IoT can create recordings"
on public.recordings;

drop policy if exists "IoT can view recordings"
on public.recordings;

drop policy if exists "IoT can update recordings"
on public.recordings;

drop policy if exists "IoT can insert readings"
on public.environmental_readings;

drop policy if exists "IoT can view readings"
on public.environmental_readings;


-- =============================================================================
-- AUTHENTICATED DASHBOARD POLICIES
-- =============================================================================


-- Profiles

create policy "Authenticated users can view profiles"

on public.profiles

for select

to authenticated

using (true);


create policy "Users can update their own profile"

on public.profiles

for update

to authenticated

using (
  auth.uid() = id
)

with check (
  auth.uid() = id
);


-- Devices

create policy "Authenticated users can view devices"

on public.devices

for select

to authenticated

using (true);


-- Recordings

create policy "Authenticated users can manage recordings"

on public.recordings

for all

to authenticated

using (true)

with check (true);


-- Environmental readings

create policy "Authenticated users can manage readings"

on public.environmental_readings

for all

to authenticated

using (true)

with check (true);


-- Assessments

create policy "Authenticated users can manage assessments"

on public.assessments

for all

to authenticated

using (true)

with check (true);


-- Settings

create policy "Authenticated users can manage settings options"

on public.settings_options

for all

to authenticated

using (true)

with check (true);


-- =============================================================================
-- NODE 2 / ESP32 IOT POLICIES
--
-- IMPORTANT:
--
-- Node 2 uses the Supabase ANON/PUBLISHABLE key.
--
-- These policies allow:
--
-- START -> INSERT recordings
-- DATA  -> INSERT environmental_readings
-- STOP  -> UPDATE recordings
--
-- FOR TESTING PURPOSES.
-- =============================================================================


-- IoT can view device

create policy "IoT can view devices"

on public.devices

for select

to anon

using (true);


-- START
-- Allow Node 2 to create recording

create policy "IoT can create recordings"

on public.recordings

for insert

to anon

with check (true);


-- Allow Node 2 to retrieve recordings if needed

create policy "IoT can view recordings"

on public.recordings

for select

to anon

using (true);


-- STOP
-- Allow Node 2 to update:
--
-- duration_seconds
-- status
--
-- Trigger automatically updates ended_at.

create policy "IoT can update recordings"

on public.recordings

for update

to anon

using (true)

with check (true);


-- DATA
-- Allow Node 2 to insert environmental readings

create policy "IoT can insert readings"

on public.environmental_readings

for insert

to anon

with check (true);


-- Allow Node 2 to view readings during testing

create policy "IoT can view readings"

on public.environmental_readings

for select

to anon

using (true);


-- =============================================================================
-- REALTIME
-- =============================================================================

-- These may already exist in the publication.
-- If you receive "already member of publication",
-- you can ignore that or remove these statements.

alter publication supabase_realtime
add table public.environmental_readings;


alter publication supabase_realtime
add table public.recordings;


-- =============================================================================
-- TEST VIEW
--
-- Makes it easy to see complete recording sessions.
-- =============================================================================

create or replace view public.recording_summary as

select

  r.id as recording_id,

  r.device_id,

  r.started_at,

  r.ended_at,

  r.duration_seconds,

  r.status,

  count(e.id) as total_packets,

  round(
    avg(e.average_co2),
    2
  ) as overall_avg_co2,

  round(
    avg(e.average_temperature),
    2
  ) as overall_avg_temperature,

  round(
    avg(e.average_noise),
    2
  ) as overall_avg_noise

from public.recordings r

left join public.environmental_readings e

on
  e.recording_id = r.id

group by

  r.id,

  r.device_id,

  r.started_at,

  r.ended_at,

  r.duration_seconds,

  r.status;


-- =============================================================================
-- TEST QUERY
-- =============================================================================

select *

from public.recording_summary

order by started_at desc;

-- =============================================================================
-- COMPLETE BANHA DATABASE RESET
-- WARNING: PERMANENTLY DELETES ALL BANHA TABLES AND DATA
-- =============================================================================


-- =============================================================================
-- 1. REMOVE TABLES FROM SUPABASE REALTIME
-- =============================================================================

do $$
begin
  alter publication supabase_realtime
  drop table public.environmental_readings;
exception
  when undefined_object then
    null;
end $$;


do $$
begin
  alter publication supabase_realtime
  drop table public.recordings;
exception
  when undefined_object then
    null;
end $$;


-- =============================================================================
-- 2. DELETE BANHA TABLES
-- =============================================================================

drop table if exists public.environmental_readings cascade;

drop table if exists public.assessments cascade;

drop table if exists public.recordings cascade;

drop table if exists public.devices cascade;

drop table if exists public.settings_options cascade;

drop table if exists public.profiles cascade;


-- =============================================================================
-- 3. DELETE BANHA FUNCTIONS
-- =============================================================================

drop function if exists public.handle_new_user() cascade;

drop function if exists public.mark_recording_assessed() cascade;


-- =============================================================================
-- 4. OPTIONAL: REMOVE UUID EXTENSION
-- Uncomment only if this is a completely separate test project
-- =============================================================================

-- drop extension if exists "uuid-ossp";


-- =============================================================================
-- DONE
-- =============================================================================

select 'BANHA database reset completed successfully.' as result;

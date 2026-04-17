(module
  (import "host" "read_param" (func $read_param (param i32) (result f32)))

  (memory (export "memory") 1)

  (data (i32.const 0)
    "effect.name=AI Phaser\n"
    "effect.title=AI Phaser\n"
    "effect.category=modulation\n"
    "effect.description=Simple stereo phaser module generated for the AudioFX WASM host.\n"
    "param.0.id=rate\n"
    "param.0.title=Rate\n"
    "param.0.slot=0\n"
    "param.0.default=0.35\n"
    "param.0.min=0.02\n"
    "param.0.max=2.5\n"
    "param.0.unit=hz\n"
    "param.1.id=depth\n"
    "param.1.title=Depth\n"
    "param.1.slot=1\n"
    "param.1.default=0.7\n"
    "param.1.min=0.0\n"
    "param.1.max=1.0\n"
    "param.1.unit=amount\n"
    "param.2.id=feedback\n"
    "param.2.title=Feedback\n"
    "param.2.slot=2\n"
    "param.2.default=0.25\n"
    "param.2.min=0.0\n"
    "param.2.max=0.85\n"
    "param.2.unit=amount\n"
    "param.3.id=mix\n"
    "param.3.title=Mix\n"
    "param.3.slot=3\n"
    "param.3.default=0.65\n"
    "param.3.min=0.0\n"
    "param.3.max=1.0\n"
    "param.3.unit=amount\n")

  (global $sample_rate (mut f32) (f32.const 48000))
  (global $phase (mut f32) (f32.const 0))
  (global $prev_wet_left (mut f32) (f32.const 0))
  (global $prev_wet_right (mut f32) (f32.const 0))

  (global $left_stage_1 (mut f32) (f32.const 0))
  (global $left_stage_2 (mut f32) (f32.const 0))
  (global $left_stage_3 (mut f32) (f32.const 0))
  (global $left_stage_4 (mut f32) (f32.const 0))
  (global $right_stage_1 (mut f32) (f32.const 0))
  (global $right_stage_2 (mut f32) (f32.const 0))
  (global $right_stage_3 (mut f32) (f32.const 0))
  (global $right_stage_4 (mut f32) (f32.const 0))

  (func $clamp (param $value f32) (param $minimum f32) (param $maximum f32) (param $fallback f32) (result f32)
    local.get $value
    local.get $value
    f32.ne
    if (result f32)
      local.get $fallback
    else
      local.get $value
      local.get $minimum
      f32.lt
      if (result f32)
        local.get $minimum
      else
        local.get $value
        local.get $maximum
        f32.gt
        if (result f32)
          local.get $maximum
        else
          local.get $value
        end
      end
    end)

  (func $wrap_phase (param $phase_value f32) (result f32)
    (local $wrapped f32)

    local.get $phase_value
    local.set $wrapped

    local.get $wrapped
    f32.const 1
    f32.ge
    if
      local.get $wrapped
      f32.const 1
      f32.sub
      local.set $wrapped
    end

    local.get $wrapped
    f32.const 0
    f32.lt
    if
      local.get $wrapped
      f32.const 1
      f32.add
      local.set $wrapped
    end

    local.get $wrapped)

  (func $triangle_from_phase (param $phase_value f32) (result f32)
    f32.const 1
    local.get $phase_value
    f32.const 2
    f32.mul
    f32.const 1
    f32.sub
    f32.abs
    f32.sub)

  (func $coefficient_from_lfo (param $triangle f32) (param $depth f32) (result f32)
    f32.const 0.15
    f32.const 0.75
    f32.const 0.5
    local.get $triangle
    f32.const 0.5
    f32.sub
    local.get $depth
    f32.mul
    f32.add
    f32.mul
    f32.add)

  (func $process_left_chain (param $input f32) (param $coefficient f32) (result f32)
    (local $x f32)
    (local $y f32)

    local.get $input
    local.set $x

    global.get $left_stage_1
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $left_stage_1
    local.get $y
    local.set $x

    global.get $left_stage_2
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $left_stage_2
    local.get $y
    local.set $x

    global.get $left_stage_3
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $left_stage_3
    local.get $y
    local.set $x

    global.get $left_stage_4
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $left_stage_4
    local.get $y)

  (func $process_right_chain (param $input f32) (param $coefficient f32) (result f32)
    (local $x f32)
    (local $y f32)

    local.get $input
    local.set $x

    global.get $right_stage_1
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $right_stage_1
    local.get $y
    local.set $x

    global.get $right_stage_2
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $right_stage_2
    local.get $y
    local.set $x

    global.get $right_stage_3
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $right_stage_3
    local.get $y
    local.set $x

    global.get $right_stage_4
    local.get $coefficient
    local.get $x
    f32.mul
    f32.sub
    local.set $y
    local.get $x
    local.get $coefficient
    local.get $y
    f32.mul
    f32.add
    global.set $right_stage_4
    local.get $y)

  (func $audiofx_reset (export "audiofx_reset")
    f32.const 0
    global.set $phase
    f32.const 0
    global.set $prev_wet_left
    f32.const 0
    global.set $prev_wet_right
    f32.const 0
    global.set $left_stage_1
    f32.const 0
    global.set $left_stage_2
    f32.const 0
    global.set $left_stage_3
    f32.const 0
    global.set $left_stage_4
    f32.const 0
    global.set $right_stage_1
    f32.const 0
    global.set $right_stage_2
    f32.const 0
    global.set $right_stage_3
    f32.const 0
    global.set $right_stage_4)

  (func (export "audiofx_prepare") (param $incoming_sample_rate f32) (param $max_block_size i32) (param $resource_slot_count i32) (result i32)
    local.get $incoming_sample_rate
    f32.const 1
    f32.const 384000
    f32.const 48000
    call $clamp
    global.set $sample_rate

    call $audiofx_reset

    i32.const 0)

  (func (export "audiofx_process") (param $in_left f32) (param $in_right f32) (result f32 f32)
    (local $rate f32)
    (local $depth f32)
    (local $feedback f32)
    (local $mix f32)
    (local $phase_value f32)
    (local $phase_right f32)
    (local $coefficient_left f32)
    (local $coefficient_right f32)
    (local $wet_left f32)
    (local $wet_right f32)
    (local $out_left f32)
    (local $out_right f32)

    i32.const 0
    call $read_param
    f32.const 0.02
    f32.const 2.5
    f32.const 0.35
    call $clamp
    local.set $rate

    i32.const 1
    call $read_param
    f32.const 0
    f32.const 1
    f32.const 0.7
    call $clamp
    local.set $depth

    i32.const 2
    call $read_param
    f32.const 0
    f32.const 0.85
    f32.const 0.25
    call $clamp
    local.set $feedback

    i32.const 3
    call $read_param
    f32.const 0
    f32.const 1
    f32.const 0.65
    call $clamp
    local.set $mix

    global.get $phase
    local.get $rate
    global.get $sample_rate
    f32.div
    f32.add
    call $wrap_phase
    local.tee $phase_value
    global.set $phase

    local.get $phase_value
    call $triangle_from_phase
    local.get $depth
    call $coefficient_from_lfo
    local.set $coefficient_left

    local.get $phase_value
    f32.const 0.25
    f32.add
    call $wrap_phase
    local.set $phase_right

    local.get $phase_right
    call $triangle_from_phase
    local.get $depth
    call $coefficient_from_lfo
    local.set $coefficient_right

    local.get $in_left
    global.get $prev_wet_left
    local.get $feedback
    f32.mul
    f32.add
    local.get $coefficient_left
    call $process_left_chain
    local.tee $wet_left
    global.set $prev_wet_left

    local.get $in_right
    global.get $prev_wet_right
    local.get $feedback
    f32.mul
    f32.add
    local.get $coefficient_right
    call $process_right_chain
    local.tee $wet_right
    global.set $prev_wet_right

    local.get $in_left
    f32.const 1
    local.get $mix
    f32.sub
    f32.mul
    local.get $wet_left
    local.get $mix
    f32.mul
    f32.add
    local.set $out_left

    local.get $in_right
    f32.const 1
    local.get $mix
    f32.sub
    f32.mul
    local.get $wet_right
    local.get $mix
    f32.mul
    f32.add
    local.set $out_right

    local.get $out_left
    local.get $out_right)

  (func (export "audiofx_get_latency_samples") (result i32)
    i32.const 0)

  (func (export "audiofx_descriptor_ptr") (result i32)
    i32.const 0)

  (func (export "audiofx_descriptor_len") (result i32)
    i32.const 653))
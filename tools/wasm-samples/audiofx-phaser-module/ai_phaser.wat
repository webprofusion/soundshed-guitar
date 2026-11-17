;; A stereo phaser written for Custom Effect ABI 2 (docs/wasm-module-authoring.md).
;;
;; Memory: the descriptor at 0, the param ids at 1024, the I/O buffer at 4096. The host writes a
;; block into the buffer (maxBlockSize left samples, then as many right), calls audiofx_process,
;; and reads the result from the same place.
(module
  (import "host" "param_handle" (func $param_handle (param i32 i32) (result i32)))
  (import "host" "param_value" (func $param_value (param i32) (result f32)))

  (memory (export "memory") 1)

  ;; The descriptor. Params are keyed by id; the order they first appear in is their order in the
  ;; UI. audiofx_descriptor_len returns its exact byte length, which build.mjs checks.
  (data (i32.const 0)
    "effect.name=AI Phaser\n"
    "effect.version=2.0.0\n"
    "effect.category=modulation\n"
    "effect.description=Simple stereo phaser module generated for the AudioFX WASM host.\n"
    "param.rate.title=Rate\n"
    "param.rate.default=0.35\n"
    "param.rate.min=0.02\n"
    "param.rate.max=2.5\n"
    "param.rate.unit=hz\n"
    "param.depth.title=Depth\n"
    "param.depth.default=0.7\n"
    "param.depth.min=0.0\n"
    "param.depth.max=1.0\n"
    "param.depth.unit=amount\n"
    "param.feedback.title=Feedback\n"
    "param.feedback.default=0.25\n"
    "param.feedback.min=0.0\n"
    "param.feedback.max=0.85\n"
    "param.feedback.unit=amount\n"
    "param.blend.title=Blend\n"
    "param.blend.default=0.65\n"
    "param.blend.min=0.0\n"
    "param.blend.max=1.0\n"
    "param.blend.unit=amount\n")

  ;; The ids audiofx_prepare looks up, each at the address and length it passes to param_handle.
  (data (i32.const 1024) "ratedepthfeedbackblend")

  (global $buffer i32 (i32.const 4096))
  (global $max_block (mut i32) (i32.const 0))

  ;; Handles from param_handle, kept from prepare. They are opaque: use what the host returns.
  (global $rate_handle (mut i32) (i32.const -1))
  (global $depth_handle (mut i32) (i32.const -1))
  (global $feedback_handle (mut i32) (i32.const -1))
  (global $blend_handle (mut i32) (i32.const -1))

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
    (global.set $phase (f32.const 0))
    (global.set $prev_wet_left (f32.const 0))
    (global.set $prev_wet_right (f32.const 0))
    (global.set $left_stage_1 (f32.const 0))
    (global.set $left_stage_2 (f32.const 0))
    (global.set $left_stage_3 (f32.const 0))
    (global.set $left_stage_4 (f32.const 0))
    (global.set $right_stage_1 (f32.const 0))
    (global.set $right_stage_2 (f32.const 0))
    (global.set $right_stage_3 (f32.const 0))
    (global.set $right_stage_4 (f32.const 0)))

  ;; 0 on success; 1 if a param id is unknown, 2 for a block size out of range, 3 if memory can't
  ;; grow to fit the buffer.
  (func (export "audiofx_prepare") (param $incoming_sample_rate f32) (param $max_block_size i32) (result i32)
    (local $pages_needed i32)

    (if (i32.or (i32.lt_s (local.get $max_block_size) (i32.const 1))
                (i32.gt_s (local.get $max_block_size) (i32.const 1048576)))
      (then (return (i32.const 2))))

    ;; The buffer needs 8 bytes a frame: a left and a right f32.
    (local.set $pages_needed
      (i32.sub
        (i32.shr_u
          (i32.add (i32.add (global.get $buffer) (i32.shl (local.get $max_block_size) (i32.const 3)))
                   (i32.const 65535))
          (i32.const 16))
        (memory.size)))
    (if (i32.gt_s (local.get $pages_needed) (i32.const 0))
      (then
        (if (i32.eq (memory.grow (local.get $pages_needed)) (i32.const -1))
          (then (return (i32.const 3))))))
    (global.set $max_block (local.get $max_block_size))

    (global.set $sample_rate
      (call $clamp (local.get $incoming_sample_rate) (f32.const 1) (f32.const 384000) (f32.const 48000)))

    (global.set $rate_handle (call $param_handle (i32.const 1024) (i32.const 4)))
    (global.set $depth_handle (call $param_handle (i32.const 1028) (i32.const 5)))
    (global.set $feedback_handle (call $param_handle (i32.const 1033) (i32.const 8)))
    (global.set $blend_handle (call $param_handle (i32.const 1041) (i32.const 5)))
    (if (i32.or
          (i32.or (i32.lt_s (global.get $rate_handle) (i32.const 0))
                  (i32.lt_s (global.get $depth_handle) (i32.const 0)))
          (i32.or (i32.lt_s (global.get $feedback_handle) (i32.const 0))
                  (i32.lt_s (global.get $blend_handle) (i32.const 0))))
      (then (return (i32.const 1))))

    (call $audiofx_reset)
    (i32.const 0))

  (func (export "audiofx_buffer_ptr") (result i32)
    (global.get $buffer))

  ;; Processes $frames (1..maxBlockSize) in place: left samples at $buffer, right ones maxBlockSize
  ;; samples further on. Params are read once per block.
  (func (export "audiofx_process") (param $frames i32)
    (local $i i32)
    (local $left_address i32)
    (local $right_offset i32)
    (local $rate f32)
    (local $depth f32)
    (local $feedback f32)
    (local $blend f32)
    (local $increment f32)
    (local $phase_value f32)
    (local $coefficient_left f32)
    (local $coefficient_right f32)
    (local $in_left f32)
    (local $in_right f32)
    (local $wet_left f32)
    (local $wet_right f32)

    (local.set $rate
      (call $clamp (call $param_value (global.get $rate_handle)) (f32.const 0.02) (f32.const 2.5) (f32.const 0.35)))
    (local.set $depth
      (call $clamp (call $param_value (global.get $depth_handle)) (f32.const 0) (f32.const 1) (f32.const 0.7)))
    (local.set $feedback
      (call $clamp (call $param_value (global.get $feedback_handle)) (f32.const 0) (f32.const 0.85) (f32.const 0.25)))
    (local.set $blend
      (call $clamp (call $param_value (global.get $blend_handle)) (f32.const 0) (f32.const 1) (f32.const 0.65)))
    (local.set $increment (f32.div (local.get $rate) (global.get $sample_rate)))
    (local.set $right_offset (i32.shl (global.get $max_block) (i32.const 2)))

    (block $done
      (loop $frame
        (br_if $done (i32.ge_s (local.get $i) (local.get $frames)))
        (local.set $left_address (i32.add (global.get $buffer) (i32.shl (local.get $i) (i32.const 2))))
        (local.set $in_left (f32.load (local.get $left_address)))
        (local.set $in_right (f32.load (i32.add (local.get $left_address) (local.get $right_offset))))

        (local.set $phase_value (call $wrap_phase (f32.add (global.get $phase) (local.get $increment))))
        (global.set $phase (local.get $phase_value))
        (local.set $coefficient_left
          (call $coefficient_from_lfo (call $triangle_from_phase (local.get $phase_value)) (local.get $depth)))
        (local.set $coefficient_right
          (call $coefficient_from_lfo
            (call $triangle_from_phase (call $wrap_phase (f32.add (local.get $phase_value) (f32.const 0.25))))
            (local.get $depth)))

        (local.set $wet_left
          (call $process_left_chain
            (f32.add (local.get $in_left) (f32.mul (global.get $prev_wet_left) (local.get $feedback)))
            (local.get $coefficient_left)))
        (global.set $prev_wet_left (local.get $wet_left))
        (local.set $wet_right
          (call $process_right_chain
            (f32.add (local.get $in_right) (f32.mul (global.get $prev_wet_right) (local.get $feedback)))
            (local.get $coefficient_right)))
        (global.set $prev_wet_right (local.get $wet_right))

        (f32.store (local.get $left_address)
          (f32.add (f32.mul (local.get $in_left) (f32.sub (f32.const 1) (local.get $blend)))
                   (f32.mul (local.get $wet_left) (local.get $blend))))
        (f32.store (i32.add (local.get $left_address) (local.get $right_offset))
          (f32.add (f32.mul (local.get $in_right) (f32.sub (f32.const 1) (local.get $blend)))
                   (f32.mul (local.get $wet_right) (local.get $blend))))

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $frame))))

  (func (export "audiofx_get_latency_samples") (result i32)
    (i32.const 0))

  (func (export "audiofx_abi_version") (result i32)
    (i32.const 2))

  (func (export "audiofx_descriptor_ptr") (result i32)
    (i32.const 0))

  (func (export "audiofx_descriptor_len") (result i32)
    (i32.const 615)))

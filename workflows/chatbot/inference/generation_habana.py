import argparse
import torch
from peft import PeftModel
from transformers import GenerationConfig, AutoModelForCausalLM, AutoModelForSeq2SeqLM, AutoTokenizer
import re, os, logging

logging.basicConfig(
    format="%(asctime)s - %(levelname)s - %(name)s - %(message)s",
    datefmt="%m/%d/%Y %H:%M:%S",
    level=logging.INFO,
)
logger = logging.getLogger(__name__)

PROMPT_DICT = {
    "prompt_with_input": (
        "Below is an instruction that describes a task, paired with an input that provides further context. "
        "Write a response that appropriately completes the request.\n\n"
        "### Instruction:\n{instruction}\n\n### Input:\n{input}\n\n### Response:\n"
    ),
    "prompt_without_input": (
        "Below is an instruction that describes a task. "
        "Write a response that appropriately completes the request.\n\n"
        "### Instruction:\n{instruction}\n\n### Response:\n"
    ),
}

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("-bm", "--base_model_path", type=str, default="")
    parser.add_argument("-pm", "--peft_model_path", type=str, default="")
    parser.add_argument(
        "-ins",
        "--instructions",
        type=str,
        nargs="+",
        default=["Tell me about alpacas.", "Tell me five words that rhyme with 'shock'."]
    )
    parser.add_argument(
        "--bf16",
        action="store_true",
        help="Whether to perform generation in bf16 precision.",
    )
    parser.add_argument(
        "--use_hpu_graphs",
        action="store_true",
        help="Whether to use HPU graphs or not. Using HPU graphs should give better latencies.",
    )
    parser.add_argument(
        "--seed",
        default=27,
        type=int,
        help="Seed to use for random generation. Useful to reproduce your runs with `--do_sample`.",
    )
    parser.add_argument(
        "--use_kv_cache",
        action="store_true",
        help="Whether to use the key/value cache for decoding. It should speed up generation.",
    )
    parser.add_argument("--local_rank", type=int, default=-1, metavar="N", help="Local process rank.")
    # Add arguments for temperature, top_p, top_k and repetition_penalty
    parser.add_argument("--temperature", type=float, default=0.1, help="The value used to control the randomness of sampling.")
    parser.add_argument("--top_p", type=float, default=0.75, help="The cumulative probability of tokens to keep for sampling.")
    parser.add_argument("--top_k", type=int, default=40, help="The number of highest probability tokens to keep for sampling.")
    parser.add_argument("--repetition_penalty", type=float, default=1.1, help="The penalty applied to repeated tokens.")
    parser.add_argument("--use_slow_tokenizer", action="store_true", help="Whether to use one of the fast tokenizer (backed by the tokenizers library) or not.")
    args = parser.parse_args()
    return args

def create_prompts(examples):
    prompts = []
    for example in examples:
        prompt_template = PROMPT_DICT["prompt_with_input"] \
            if example["input"] != "" else PROMPT_DICT["prompt_without_input"]
        prompt = prompt_template.format_map(example)
        prompts.append(prompt)
    return prompts

def main():
    args = parse_args()
    print(args)
    base_model_path = args.base_model_path
    peft_model_path = args.peft_model_path
    prompts = create_prompts(
        [{'instruction':instruction, 'input':''} for instruction in args.instructions]
    )

    # Check the validity of the arguments
    if not 0 < args.temperature <= 1.0:
        raise ValueError("Temperature must be between 0 and 1.")
    if not 0 <= args.top_p <= 1.0:
        raise ValueError("Top-p must be between 0 and 1.")
    if not 0 <= args.top_k <= 200:
        raise ValueError("Top-k must be between 0 and 200.")
    if not 1.0 <= args.repetition_penalty <= 2.0:
        raise ValueError("Repetition penalty must be between 1 and 2.")

    # If the DeepSpeed launcher is used, the env variable _ will be equal to /usr/local/bin/deepspeed
    # For multi node, the value of the env variable WORLD_SIZE should be larger than 8
    use_deepspeed = "deepspeed" in os.environ["_"] or (
        "WORLD_SIZE" in os.environ and int(os.environ["WORLD_SIZE"]) > 8
    )
    if use_deepspeed:
        # Set necessary env variables
        os.environ.setdefault("PT_HPU_LAZY_ACC_PAR_MODE", "0")
        os.environ.setdefault("PT_HPU_ENABLE_LAZY_COLLECTIVES", "true")

    # Device is HPU
    args.device = "hpu"
    import habana_frameworks.torch.hpu as torch_hpu

    # Get world size, rank and local rank
    from habana_frameworks.torch.distributed.hccl import initialize_distributed_hpu

    world_size, rank, args.local_rank = initialize_distributed_hpu()

    if use_deepspeed:
        # Check if DeepSpeed is installed
        from transformers.deepspeed import is_deepspeed_available

        if not is_deepspeed_available():
            raise ImportError(
                "This script requires deepspeed: `pip install"
                " git+https://github.com/HabanaAI/DeepSpeed.git@1.10.0`."
            )
        import deepspeed

        # Initialize process(es) for DeepSpeed
        deepspeed.init_distributed(dist_backend="hccl")
        logger.info("DeepSpeed is enabled.")
    else:
        logger.info("Single-device run.")

    # Tweak generation so that it runs faster on Gaudi
    from optimum.habana.transformers.modeling_utils import adapt_transformers_to_gaudi

    adapt_transformers_to_gaudi()
    # Set seed before initializing model.
    from optimum.habana.utils import set_seed

    set_seed(args.seed)

    if use_deepspeed or args.bf16:
        model_dtype = torch.bfloat16
    else:
        model_dtype = torch.float


    tokenizer = AutoTokenizer.from_pretrained(base_model_path, use_fast=True)
    if use_deepspeed:
        with deepspeed.OnDevice(dtype=model_dtype, device=args.device):
            if re.search("flan-t5", base_model_path, re.IGNORECASE):
                model = AutoModelForSeq2SeqLM.from_pretrained(base_model_path, torch_dtype=model_dtype)
            elif re.search("llama", base_model_path, re.IGNORECASE):
                model = AutoModelForCausalLM.from_pretrained(base_model_path, torch_dtype=model_dtype)
            else:
                raise ValueError(f"Unsupported model {base_model_path}, only supports FLAN-T5 and LLAMA now.")

            if re.search("llama", model.config.architectures[0], re.IGNORECASE):
                # unwind broken decapoda-research config
                model.generation_config.pad_token_id = 0
                model.generation_config.bos_token_id = 1
                model.generation_config.eos_token_id = 2

            if hasattr(model.generation_config, "pad_token_id"):
                tokenizer.pad_token_id = model.generation_config.pad_token_id
            if hasattr(model.generation_config, "eos_token_id"):
                tokenizer.eos_token_id = model.generation_config.eos_token_id
            if hasattr(model.generation_config, "bos_token_id"):
                tokenizer.bos_token_id = model.generation_config.bos_token_id

            if peft_model_path:
                model = PeftModel.from_pretrained(model, peft_model_path)

            model = model.eval()
            # Initialize the model
            ds_inference_kwargs = {"dtype": model_dtype}
            ds_inference_kwargs["tensor_parallel"] = {"tp_size": 8}
            ds_inference_kwargs["enable_cuda_graph"] = args.use_hpu_graphs
            # Make sure all devices/nodes have access to the model checkpoints
            torch.distributed.barrier()
            model = deepspeed.init_inference(model, **ds_inference_kwargs)
            model = model.module
    else:
        if re.search("flan-t5", base_model_path, re.IGNORECASE):
            model = AutoModelForSeq2SeqLM.from_pretrained(base_model_path, torch_dtype=model_dtype)
        elif re.search("llama", base_model_path, re.IGNORECASE):
            model = AutoModelForCausalLM.from_pretrained(base_model_path, torch_dtype=model_dtype)
        elif re.search("mpt", base_model_path, re.IGNORECASE):
            model = AutoModelForCausalLM.from_pretrained(base_model_path, torch_dtype=model_dtype,
                                                         low_cpu_mem_usage=True, trust_remote_code=True)
        else:
            raise ValueError(f"Unsupported model {base_model_path}, only supports FLAN-T5 and LLAMA now.")
        model = model.eval().to(args.device)

        if args.use_hpu_graphs:
            from habana_frameworks.torch.hpu import wrap_in_hpu_graph

            model = wrap_in_hpu_graph(model)

        if re.search("llama", model.config.architectures[0], re.IGNORECASE):
            # unwind broken decapoda-research config
            model.generation_config.pad_token_id = 0
            model.generation_config.bos_token_id = 1
            model.generation_config.eos_token_id = 2

        if hasattr(model.generation_config, "pad_token_id"):
            tokenizer.pad_token_id = model.generation_config.pad_token_id
        if hasattr(model.generation_config, "eos_token_id"):
            tokenizer.eos_token_id = model.generation_config.eos_token_id
        if hasattr(model.generation_config, "bos_token_id"):
            tokenizer.bos_token_id = model.generation_config.bos_token_id

    if rank in [-1, 0]:
        logger.info(f"Args: {args}")
        logger.info(f"device: {args.device}, n_hpu: {world_size}, bf16: {use_deepspeed or args.bf16}")

    def evaluate(
        prompt,
        temperature,
        top_p,
        top_k,
        repetition_penalty,
        num_beams=4,
        max_new_tokens=128,
        **kwargs,
    ):
        input = tokenizer(prompt, return_tensors="pt")
        input_ids = input["input_ids"].to(model.device)
        generation_config = GenerationConfig(
            use_cache=args.use_kv_cache,
            temperature=temperature,
            top_p=top_p,
            top_k=top_k,
            repetition_penalty=repetition_penalty,
            num_beams=num_beams,
            **kwargs,
        )
        with torch.no_grad():
            generation_output = model.generate(
                input_ids=input_ids,
                generation_config=generation_config,
                return_dict_in_generate=True,
                output_scores=True,
                max_new_tokens=max_new_tokens,
                lazy_mode=True,
                hpu_graphs=args.use_hpu_graphs,
            )
        sequence = generation_output.sequences[0]
        output = tokenizer.decode(sequence, skip_special_tokens=True)
        if "### Response:" in output:
            return output.split("### Response:")[1].strip()
        elif "<pad> " in output:
            return output.split("<pad> ")[1].strip()
        else:
            return output

    torch_hpu.synchronize()
    for idx, tp in enumerate(zip(prompts, args.instructions)):
        prompt, instruction = tp
        idxs = f"{idx+1}"
        logger.info("="*30 + idxs + "="*30)
        logger.info(f"Instruction: {instruction}")
        response = evaluate(prompt,
                            temperature=args.temperature,
                            top_p=args.top_p,
                            top_k=args.top_k,
                            repetition_penalty=args.repetition_penalty)
        logger.info(f"Response: {response}")
        logger.info("="*(60 + len(idxs)))

if __name__ == "__main__":
    main()

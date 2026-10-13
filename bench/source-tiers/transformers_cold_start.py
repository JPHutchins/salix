import importlib
import tempfile
import time

started = time.perf_counter()
importlib.import_module("torch")
torch_imported = time.perf_counter()
transformers = importlib.import_module("transformers")
imported = time.perf_counter()
outputs = importlib.import_module("transformers.modeling_outputs")
outputs_defined = time.perf_counter()
training_args = importlib.import_module("transformers.training_args")
arguments_defined = time.perf_counter()
importlib.import_module("transformers.models.bert.modeling_bert")
model_defined = time.perf_counter()
directory = tempfile.mkdtemp()
arguments = training_args.TrainingArguments(output_dir=directory)
constructed = time.perf_counter()
parser = transformers.HfArgumentParser(training_args.TrainingArguments)
(parsed,) = parser.parse_args_into_dataclasses(["--output_dir", directory, "--learning_rate", "3e-5"])
finished = time.perf_counter()

assert arguments.output_dir == directory
assert parsed.learning_rate == 3e-5
assert outputs.BaseModelOutput(last_hidden_state=None).last_hidden_state is None
print(
    f"import torch {1e3 * (torch_imported - started):.3f} ms\n"
    f"import transformers {1e3 * (imported - torch_imported):.3f} ms\n"
    f"define model outputs {1e3 * (outputs_defined - imported):.3f} ms\n"
    f"define TrainingArguments {1e3 * (arguments_defined - outputs_defined):.3f} ms\n"
    f"define bert modeling {1e3 * (model_defined - arguments_defined):.3f} ms\n"
    f"construct TrainingArguments {1e3 * (constructed - model_defined):.3f} ms\n"
    f"parse TrainingArguments {1e3 * (finished - constructed):.3f} ms\n"
    f"total after torch {1e3 * (finished - torch_imported):.3f} ms"
)

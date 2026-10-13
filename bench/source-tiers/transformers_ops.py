import importlib
import tempfile
import time
from collections.abc import Callable

torch = importlib.import_module("torch")
transformers = importlib.import_module("transformers")
outputs = importlib.import_module("transformers.modeling_outputs")
training_args = importlib.import_module("transformers.training_args")
bert_config = importlib.import_module("transformers.models.bert.configuration_bert").BertConfig
directory = tempfile.mkdtemp()
tensor = torch.zeros(1, 2, 3)
output = outputs.BaseModelOutput(last_hidden_state=tensor)
parser = transformers.HfArgumentParser(training_args.TrainingArguments)
config = bert_config()


def per_iteration(label: str, iterations: int, operation: Callable[[], object]) -> None:
    operation()
    started = time.perf_counter()
    for _ in range(iterations):
        operation()
    print(f"{label} {1e6 * (time.perf_counter() - started) / iterations:.3f} us/iter")


per_iteration("construct BaseModelOutput", 20000, lambda: outputs.BaseModelOutput(last_hidden_state=tensor))
per_iteration("BaseModelOutput.to_tuple", 20000, output.to_tuple)
per_iteration("construct BertConfig", 2000, bert_config)
per_iteration("BertConfig.to_dict", 2000, config.to_dict)
per_iteration("construct TrainingArguments", 200, lambda: training_args.TrainingArguments(output_dir=directory))
per_iteration(
    "parse TrainingArguments",
    100,
    lambda: parser.parse_args_into_dataclasses(["--output_dir", directory, "--learning_rate", "3e-5"]),
)

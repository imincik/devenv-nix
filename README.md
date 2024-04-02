# Create a new Devenv Nix release

* Sync source code with Nix upstream

* Create new branch from new Nix release tag
```
  git branch devenv-<TAG> <TAG>
  git checkout devenv-<TAG>
```

* Pull patches
```
  git checkout devenv-patches -- patches-list.txt
  git checkout devenv-patches -- patches
```

* Check if patches apply
```
  while read p; do git apply --check $p; done < patches-list.txt
```

* Apply patches
```
  while read p; do git apply $p; done < patches-list.txt
```
